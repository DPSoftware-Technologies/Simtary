#include "render/LensFlare.h"
#include "imgui.h"
#include <algorithm>

namespace st {

namespace {

// 0 below `lo`, 1 above `hi`, linear in between. Tolerates lo >= hi (becomes a step).
float LinearStep(float lo, float hi, float x) {
	if (hi - lo < 1e-6f) return x < lo ? 0.0f : 1.0f;
	return wi::math::saturate((x - lo) / (hi - lo));
}

} // namespace

void LensFlare::Init() {
	// Same load path as the ImGui shaders: by the time Initialize() runs,
	// wi::renderer::GetShaderPath() already has the backend subfolder appended, so
	// these resolve to "<exe>/shaders/hlsl6/" on DX12 and "<exe>/shaders/spirv/" on
	// Vulkan. The CMake post-build step writes the .cso files there.
	const bool vsOK = wi::renderer::LoadShader(wi::graphics::ShaderStage::VS, vs_, "StLensFlareVS.cso");
	const bool psOK = wi::renderer::LoadShader(wi::graphics::ShaderStage::PS, ps_, "StLensFlarePS.cso");
	if (!vsOK || !psOK) {
		wi::backlog::post(
			"[LensFlare] failed to load StLensFlareVS.cso / StLensFlarePS.cso from " +
			wi::renderer::GetShaderPath() +
			". Build with dxc available. Lens flare disabled.",
			wi::backlog::LogLevel::Error);
		return;
	}

	wi::graphics::PipelineStateDesc desc;
	desc.vs = &vs_;
	desc.ps = &ps_;
	desc.il = nullptr; // fullscreen triangle comes from SV_VertexID, no vertex buffer
	// Drawn over the already-composed frame: depth is irrelevant, and it adds light
	// to the image rather than replacing it.
	desc.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHDISABLED);
	desc.rs = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
	desc.bs = wi::renderer::GetBlendState(wi::enums::BSTYPE_ADDITIVE);
	desc.pt = wi::graphics::PrimitiveTopology::TRIANGLELIST;

	wi::graphics::GetDevice()->CreatePipelineState(&desc, &pso_);
}

void LensFlare::Update(const wi::scene::Scene& scene, const wi::scene::CameraComponent& camera, float dt) {
	time_ += dt;

	// occlusion == 0 makes Draw() skip entirely, so it doubles as the "nothing to
	// show this frame" signal for every early-out below.
	constants_.occlusion = 0.0f;
	if (!settings.enabled || !pso_.IsValid()) return;

	XMFLOAT3 sunColor = XMFLOAT3(1.0f, 1.0f, 1.0f);

	if (settings.followSun) {
		// Brightest directional light wins, mirroring how the engine picks the light
		// that drives the sky (weather.most_important_light_index in wiScene.cpp).
		const wi::scene::LightComponent* sun = nullptr;
		for (size_t i = 0; i < scene.lights.GetCount(); ++i) {
			const wi::scene::LightComponent& light = scene.lights[i];
			if (light.type != wi::scene::LightComponent::DIRECTIONAL) continue;
			if (sun == nullptr || light.intensity > sun->intensity) sun = &light;
		}
		if (sun == nullptr) return;

		// LightComponent::direction on a directional light points *towards* the light
		// (it is what the engine feeds to weather.sunDirection), which is exactly the
		// direction we want to project.
		sunDirection = sun->direction;
		sunColor = sun->color;
	}

	// Project the sun. It is directional - infinitely far away - so transform it as a
	// point at infinity (w = 0) rather than picking an arbitrary large distance: the
	// result is exact and can never be clipped by the far plane.
	const XMVECTOR clip = XMVector4Transform(
		XMVectorSet(sunDirection.x, sunDirection.y, sunDirection.z, 0.0f),
		camera.GetViewProjection());

	// clip.w is the view-space depth of the direction; <= 0 means the sun is behind
	// the camera, where dividing through would mirror it onto the screen.
	const float w = XMVectorGetW(clip);
	if (w <= 1e-6f) return;

	// NDC -> UV. Only xy matter, so the engine's reverse-Z depth range is irrelevant.
	const float ndcX = XMVectorGetX(clip) / w;
	const float ndcY = XMVectorGetY(clip) / w;
	const XMFLOAT2 sunUV(ndcX * 0.5f + 0.5f, -ndcY * 0.5f + 0.5f);

	// Fades
	// Off the edge of the frame: distance from the [0,1] UV box. Ghosts linger a
	// little past the edge in a real lens, hence the soft ramp rather than a cut.
	const float outX = std::max(0.0f, std::max(-sunUV.x, sunUV.x - 1.0f));
	const float outY = std::max(0.0f, std::max(-sunUV.y, sunUV.y - 1.0f));
	const float outside = std::sqrt(outX * outX + outY * outY);
	const float edgeFade = 1.0f - LinearStep(0.0f, std::max(1e-4f, settings.offscreenFade), outside);

	// Below the horizon there is no sun to flare, whatever the projection says.
	const float horizonFade = LinearStep(settings.horizonFadeLow, settings.horizonFadeHigh, sunDirection.y);

	const float occlusion = edgeFade * horizonFade;
	if (occlusion <= 0.001f) return;

	// Pack the constant buffer
	const float height = std::max(1.0f, camera.height);

	constants_.sunUV = sunUV;
	constants_.aspect = std::max(1e-4f, camera.width / height);
	constants_.intensity = settings.intensity;
	constants_.tint = settings.tintFromSun ? sunColor : settings.tint;
	constants_.ghostSpacing = settings.ghostSpacing;
	constants_.ghostCount = (float)settings.ghostCount;
	constants_.haloWidth = settings.haloWidth;
	constants_.streakIntensity = settings.streakIntensity;
	constants_.glowIntensity = settings.glowIntensity;
	constants_.chromaOffset = settings.chromaOffset;
	constants_.starburstIntensity = settings.starburstIntensity;
	constants_.time = time_;
	constants_.occlusion = occlusion;
}

void LensFlare::Draw(const wi::Canvas& canvas, wi::graphics::CommandList cmd) {
	// Update() zeroes occlusion whenever there is nothing to draw (disabled, no sun,
	// sun behind the camera, fully faded), so this one test covers every case.
	if (constants_.occlusion <= 0.001f || !pso_.IsValid()) return;

	wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
	device->EventBegin("LensFlare", cmd);

	const float width = (float)canvas.GetPhysicalWidth();
	const float height = (float)canvas.GetPhysicalHeight();

	wi::graphics::Viewport vp;
	vp.width = width;
	vp.height = height;
	device->BindViewports(1, &vp, cmd);

	// The render path leaves its own scissor behind; reset to the full frame or the
	// triangle gets clipped to whatever was drawn last.
	wi::graphics::Rect scissor;
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = (int32_t)canvas.GetPhysicalWidth();
	scissor.bottom = (int32_t)canvas.GetPhysicalHeight();
	device->BindScissorRects(1, &scissor, cmd);

	device->BindPipelineState(&pso_, cmd);
	device->BindDynamicConstantBuffer(constants_, 0, cmd);
	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}

void LensFlare::SaveTo(st::nbt::Tag& out) const {
	const Settings& s = settings;
	out.putBool ("enabled", s.enabled);
	out.putBool ("followSun", s.followSun);
	out.putBool ("tintFromSun", s.tintFromSun);
	out.putFloat("intensity", s.intensity);
	out.putFloat("tintR", s.tint.x);
	out.putFloat("tintG", s.tint.y);
	out.putFloat("tintB", s.tint.z);
	out.putFloat("glowIntensity", s.glowIntensity);
	out.putFloat("streakIntensity", s.streakIntensity);
	out.putFloat("starburstIntensity", s.starburstIntensity);
	out.putInt  ("ghostCount", s.ghostCount);
	out.putFloat("ghostSpacing", s.ghostSpacing);
	out.putFloat("haloWidth", s.haloWidth);
	out.putFloat("chromaOffset", s.chromaOffset);
	out.putFloat("horizonFadeLow", s.horizonFadeLow);
	out.putFloat("horizonFadeHigh", s.horizonFadeHigh);
	out.putFloat("offscreenFade", s.offscreenFade);
	// Manual sun direction - only meaningful with followSun off, but harmless to keep.
	out.putFloat("sunDirX", sunDirection.x);
	out.putFloat("sunDirY", sunDirection.y);
	out.putFloat("sunDirZ", sunDirection.z);
}

void LensFlare::LoadFrom(const st::nbt::Tag& in) {
	Settings s; // start from struct defaults so a missing key keeps its default
	s.enabled            = in.getBool ("enabled", s.enabled);
	s.followSun          = in.getBool ("followSun", s.followSun);
	s.tintFromSun        = in.getBool ("tintFromSun", s.tintFromSun);
	s.intensity          = in.getFloat("intensity", s.intensity);
	s.tint.x             = in.getFloat("tintR", s.tint.x);
	s.tint.y             = in.getFloat("tintG", s.tint.y);
	s.tint.z             = in.getFloat("tintB", s.tint.z);
	s.glowIntensity      = in.getFloat("glowIntensity", s.glowIntensity);
	s.streakIntensity    = in.getFloat("streakIntensity", s.streakIntensity);
	s.starburstIntensity = in.getFloat("starburstIntensity", s.starburstIntensity);
	s.ghostCount         = in.getInt  ("ghostCount", s.ghostCount);
	s.ghostSpacing       = in.getFloat("ghostSpacing", s.ghostSpacing);
	s.haloWidth          = in.getFloat("haloWidth", s.haloWidth);
	s.chromaOffset       = in.getFloat("chromaOffset", s.chromaOffset);
	s.horizonFadeLow     = in.getFloat("horizonFadeLow", s.horizonFadeLow);
	s.horizonFadeHigh    = in.getFloat("horizonFadeHigh", s.horizonFadeHigh);
	s.offscreenFade      = in.getFloat("offscreenFade", s.offscreenFade);
	settings = s;

	sunDirection.x = in.getFloat("sunDirX", sunDirection.x);
	sunDirection.y = in.getFloat("sunDirY", sunDirection.y);
	sunDirection.z = in.getFloat("sunDirZ", sunDirection.z);
}

void LensFlare::GUI() {
	if (!pso_.IsValid()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Shaders failed to load - see BackLog.");
		return;
	}

	ImGui::Checkbox("Enabled", &settings.enabled);
	ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 4.0f);

	ImGui::SeparatorText("Source");
	ImGui::Checkbox("Follow Sun", &settings.followSun);
	if (!settings.followSun) {
		ImGui::SliderFloat3("Sun Direction", &sunDirection.x, -1.0f, 1.0f);
	}
	ImGui::Checkbox("Tint From Sun", &settings.tintFromSun);
	if (!settings.tintFromSun) {
		ImGui::ColorEdit3("Tint", &settings.tint.x);
	}

	ImGui::SeparatorText("Layers");
	ImGui::SliderFloat("Glow", &settings.glowIntensity, 0.0f, 3.0f);
	ImGui::SliderFloat("Streak", &settings.streakIntensity, 0.0f, 3.0f);
	ImGui::SliderFloat("Starburst", &settings.starburstIntensity, 0.0f, 3.0f);
	ImGui::SliderInt("Ghost Count", &settings.ghostCount, 0, 16);
	ImGui::SliderFloat("Ghost Spacing", &settings.ghostSpacing, 0.05f, 1.0f);
	ImGui::SliderFloat("Halo Width", &settings.haloWidth, 0.0f, 1.0f);
	ImGui::SliderFloat("Chromatic Split", &settings.chromaOffset, 0.0f, 0.05f, "%.4f");

	ImGui::SeparatorText("Fades");
	ImGui::SliderFloat("Horizon Low", &settings.horizonFadeLow, -0.5f, 0.5f);
	ImGui::SliderFloat("Horizon High", &settings.horizonFadeHigh, -0.5f, 0.5f);
	ImGui::SliderFloat("Offscreen Fade", &settings.offscreenFade, 0.01f, 1.0f);

	ImGui::SeparatorText("State");
	ImGui::Text("Sun UV:    %.3f, %.3f", constants_.sunUV.x, constants_.sunUV.y);
	ImGui::Text("Occlusion: %.3f", constants_.occlusion);
}

} // namespace st
