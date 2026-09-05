#include "stAudioComponents.h"
#include "wiScene.h"
#include "wiRenderer.h"
#include "wiBacklog.h"

#include <algorithm>
#include <cmath>

using namespace wi::ecs;
using namespace wi::scene;

namespace st
{
	namespace
	{
		// Read the entity's world transform into the form the spatializer wants.
		// GetPosition()/GetForward()/GetUp() come off the world matrix, which is
		// already rebased around the render origin - the right space for audio, since
		// only the emitter-to-listener vector matters and both ends share the origin.
		bool ReadTransform(TransformComponent* transform, audio::SpatialTransform& out)
		{
			if (transform == nullptr)
				return false;
			out.position = transform->GetPosition();
			out.forward = transform->GetForward();
			out.up = transform->GetUp();
			return true;
		}

		template<typename E>
		E ToEnum(int value, E fallback, int count)
		{
			return (value >= 0 && value < count) ? (E)value : fallback;
		}

		inline XMFLOAT3 Neg(const XMFLOAT3& v) { return XMFLOAT3(-v.x, -v.y, -v.z); }

		// Point `out` down one of the entity's local axes. The numbering matches
		// st::LocalAxes exactly; see the note on AudioEmitterComponent::forwardAxis for
		// why it is duplicated here rather than shared.
		void ApplyForwardAxis(TransformComponent* transform, int axis, audio::SpatialTransform& out)
		{
			if (transform == nullptr)
				return;
			const XMFLOAT3 fwd = transform->GetForward();   // +Z
			const XMFLOAT3 up = transform->GetUp();         // +Y
			const XMFLOAT3 right = transform->GetRight();   // +X
			switch (axis)
			{
			case 1: out.forward = Neg(fwd);   out.up = up;        break;  // -Z
			case 2: out.forward = Neg(up);    out.up = fwd;       break;  // -Y (a spot light)
			case 3: out.forward = up;         out.up = Neg(fwd);  break;  // +Y
			case 4: out.forward = right;      out.up = up;        break;  // +X
			case 5: out.forward = Neg(right); out.up = up;        break;  // -X
			default: out.forward = fwd;       out.up = up;        break;  // +Z
			}
		}

		// Steam Audio's dipole directivity, evaluated for the debug overlay so the
		// drawing and the DSP cannot disagree about the shape:
		//     gain = |(1 - weight) + weight * cos(theta)| ^ power
		// theta is the angle between the speaker's forward axis and the direction to the
		// listener. weight 0 gives 1 in every direction - a sphere; weight 1 gives
		// |cos|^power, the figure of eight.
		float DirectivityGain(float cosTheta, float weight, float power)
		{
			const float lobe = std::fabs((1.0f - weight) + weight * cosTheta);
			return power == 1.0f ? lobe : std::pow(lobe, power);
		}
	}

	// AudioEmitterComponent

	void AudioEmitterComponent::Start()
	{
		// Metadata binding. Every field is optional; an argument that is absent leaves
		// the C++ default in place, so an emitter attached with no arguments at all is
		// a plain omnidirectional point source.
		Bind(clip, "clip");
		Bind(playOnStart, "playOnStart");
		Bind(loop, "loop");
		Bind(volume, "volume");
		Bind(pitch, "pitch");
		Bind(startDelay, "startDelay");
		Bind(submix, "submix");
		Bind(spatial, "spatial");
		Bind(followTransform, "followTransform");
		Bind(computeVelocity, "computeVelocity");

		Bind(distanceModel, "distanceModel");
		Bind(minDistance, "minDistance");
		Bind(maxDistance, "maxDistance");
		Bind(airAbsorption, "airAbsorption");
		Bind(airAbsorptionLow, "airAbsorptionLow");
		Bind(airAbsorptionMid, "airAbsorptionMid");
		Bind(airAbsorptionHigh, "airAbsorptionHigh");

		Bind(forwardAxis, "forwardAxis");
		Bind(dipoleWeight, "dipoleWeight");
		Bind(dipolePower, "dipolePower");
		Bind(debugDraw, "debugDraw");
		Bind(debugScale, "debugScale");

		Bind(occlusion, "occlusion");
		Bind(occlusionRadius, "occlusionRadius");
		Bind(occlusionSamples, "occlusionSamples");
		Bind(transmission, "transmission");
		Bind(transmissionRays, "transmissionRays");

		Bind(reflections, "reflections");
		Bind(reflectionsMix, "reflectionsMix");
		Bind(pathing, "pathing");
		Bind(pathingVisSamples, "pathingVisSamples");
		Bind(pathingVisRadius, "pathingVisRadius");
		Bind(pathingVisThreshold, "pathingVisThreshold");
		Bind(pathingRange, "pathingRange");
		Bind(pathingValidation, "pathingValidation");

		Bind(output, "output");
		Bind(interpolation, "interpolation");
		Bind(spatialBlend, "spatialBlend");
		Bind(applyDistanceAttenuation, "applyDistanceAttenuation");
		Bind(applyAirAbsorption, "applyAirAbsorption");
		Bind(applyDirectivity, "applyDirectivity");
		Bind(applyOcclusion, "applyOcclusion");
		Bind(applyTransmission, "applyTransmission");

		audio::AudioEngine& engine = audio::AudioEngine::Get();
		if (!engine.IsInitialized())
		{
			wilog_warning("stAudioEmitter: the audio engine is not running - emitter on entity %llu is inert.",
				(unsigned long long)entity);
			return;
		}

		emitter_ = engine.CreateEmitter(componentName + "#" + std::to_string(localID));
		SyncSettings();
		SyncClip();

		// Seed the transform before the first block so the emitter is never heard at
		// the origin for one frame.
		audio::SpatialTransform t;
		if (ReadTransform(GetComponent<TransformComponent>(), t))
		{
			emitter_->SetTransform(t);
			lastPosition_ = t.position;
			hasLastPosition_ = true;
		}

		// SyncClip already started an immediate playback; only the delayed case is left.
		if (playOnStart && startDelay > 0.0f)
			emitter_->PlayDelayed(startDelay);
	}

	void AudioEmitterComponent::SyncClip()
	{
		if (!emitter_ || clip == loadedClip_)
			return;

		// Recorded BEFORE the load so a path that cannot be read is logged once rather
		// than retried on every frame for the rest of the session.
		loadedClip_ = clip;

		if (clip.empty())
		{
			emitter_->Stop();
			return;
		}

		// LoadClip decodes the whole file up front, so a long asset costs a stall here.
		// This runs on a job-system worker (Update is parallel by default), not the main
		// thread, and only on the frame the path actually changes.
		audio::AudioClip loaded = audio::LoadClip(clip);
		if (!loaded)
		{
			wilog_warning("stAudioEmitter: could not load \"%s\" for entity %llu.",
				clip.c_str(), (unsigned long long)entity);
			return;
		}

		emitter_->SetClip(loaded);
		emitter_->Stop();   // rewind to the start of the play region
		if (playOnStart)
			emitter_->Play();
	}

	void AudioEmitterComponent::SyncSettings()
	{
		if (!emitter_)
			return;

		emitter_->SetVolume(volume);
		emitter_->SetPitch(pitch);
		emitter_->SetLoop(loop);
		emitter_->SetSpatial(spatial);
		emitter_->SetSubmix(ToEnum(submix, audio::Submix::SoundEffect, (int)audio::Submix::Count));

		audio::EmitterSpatialSettings& s = emitter_->SpatialSettings();
		s.distanceModel = ToEnum(distanceModel, audio::DistanceAttenuationModel::Default, 2);
		s.minDistance = std::max(0.01f, minDistance);
		s.maxDistance = std::max(s.minDistance, maxDistance);
		s.airAbsorption = ToEnum(airAbsorption, audio::AirAbsorptionModel::None, 3);
		s.airAbsorptionCoefficients[0] = airAbsorptionLow;
		s.airAbsorptionCoefficients[1] = airAbsorptionMid;
		s.airAbsorptionCoefficients[2] = airAbsorptionHigh;

		s.dipoleWeight = std::clamp(dipoleWeight, 0.0f, 1.0f);
		s.dipolePower = std::max(0.0f, dipolePower);
		// A lobe that is set but not applied is the most confusing state this component
		// can be in - the inspector says "directional" and the sound is omni. Any
		// non-zero weight turns the direct-path directivity on by itself.
		applyDirectivity = applyDirectivity || s.dipoleWeight > 0.0f;

		s.occlusion = ToEnum(occlusion, audio::OcclusionMode::Off, 3);
		s.occlusionRadius = std::max(0.0f, occlusionRadius);
		s.occlusionSamples = std::max(1, occlusionSamples);
		s.transmission = ToEnum(transmission, audio::TransmissionMode::Off, 3);
		s.transmissionRays = std::max(1, transmissionRays);

		s.reflections = reflections;
		s.reflectionsMix = std::clamp(reflectionsMix, 0.0f, 1.0f);
		s.pathing = pathing;
		s.pathingVisSamples = std::max(1, pathingVisSamples);
		s.pathingVisRadius = std::max(0.0f, pathingVisRadius);
		s.pathingVisThreshold = std::clamp(pathingVisThreshold, 0.0f, 1.0f);
		s.pathingRange = std::max(0.0f, pathingRange);
		s.pathingValidation = pathingValidation;

		s.output = ToEnum(output, audio::SpatialOutput::Binaural, 3);
		s.interpolation = ToEnum(interpolation, audio::HRTFInterpolation::Bilinear, 2);
		s.spatialBlend = std::clamp(spatialBlend, 0.0f, 1.0f);
		s.applyDistanceAttenuation = applyDistanceAttenuation;
		s.applyAirAbsorption = applyAirAbsorption;
		s.applyDirectivity = applyDirectivity;
		s.applyOcclusion = applyOcclusion;
		s.applyTransmission = applyTransmission;

		emitter_->ApplySpatialSettings();
	}

	void AudioEmitterComponent::Update(float dt)
	{
		if (!emitter_)
			return;

		// Pushing settings every frame keeps a live-edited field (inspector, gameplay
		// code writing the member directly) working without anyone remembering to call
		// SyncSettings. It is a handful of stores into atomics and one struct copy.
		SyncSettings();
		// And the clip, which is the one field that costs something to apply - hence the
		// changed-path guard inside rather than a reload every frame.
		SyncClip();

		if (!followTransform)
			return;

		TransformComponent* transform = GetComponent<TransformComponent>();
		audio::SpatialTransform t;
		if (!ReadTransform(transform, t))
			return;
		// Aim the speaker down the chosen local axis. This is what makes the directivity
		// lobe point somewhere meaningful: Steam Audio measures the dipole against
		// `forward`, so choosing the axis IS choosing where the speaker faces.
		ApplyForwardAxis(transform, forwardAxis, t);

		if (computeVelocity && hasLastPosition_ && dt > 0.0f)
		{
			t.velocity = XMFLOAT3(
				(t.position.x - lastPosition_.x) / dt,
				(t.position.y - lastPosition_.y) / dt,
				(t.position.z - lastPosition_.z) / dt);
		}
		lastPosition_ = t.position;
		hasLastPosition_ = true;

		emitter_->SetTransform(t);

		if (debugDraw)
			DrawDirectivity(t);
	}

	void AudioEmitterComponent::DrawDirectivity(const audio::SpatialTransform& t)
	{
		// Build an orthonormal basis around the speaker's forward axis. `up` comes from
		// ApplyForwardAxis and is already perpendicular, but it is re-derived here so a
		// drifted transform cannot skew the drawing.
		const XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&t.forward));
		XMVECTOR right = XMVector3Cross(fwd, XMVector3Normalize(XMLoadFloat3(&t.up)));
		if (XMVectorGetX(XMVector3Length(right)) < 1e-5f)
			right = XMVector3Cross(fwd, XMVectorSet(0, 0, 1, 0));
		right = XMVector3Normalize(right);
		const XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, fwd));
		const XMVECTOR origin = XMLoadFloat3(&t.position);

		const float weight = std::clamp(dipoleWeight, 0.0f, 1.0f);
		const float power = std::max(0.0f, dipolePower);
		const float scale = std::max(0.01f, debugScale);

		// Green while the emitter is actually radiating, dim grey otherwise: a lobe on a
		// silent speaker should not read the same as one on a live one.
		const bool live = IsPlaying();
		const XMFLOAT4 lobeColour = live ? XMFLOAT4(0.30f, 0.90f, 0.45f, 1.0f)
		                                 : XMFLOAT4(0.45f, 0.45f, 0.50f, 1.0f);

		constexpr int kSegments = 48;
		wi::vector<wi::renderer::RenderableLine> lines;
		lines.reserve(kSegments * 2 + 8);

		// Two perpendicular slices through the lobe - the horizontal plane (forward x
		// right) and the vertical one (forward x up). Two outlines read as a 3D shape
		// where one reads as a flat curve, and they cost 96 lines instead of a mesh.
		for (int plane = 0; plane < 2; ++plane)
		{
			const XMVECTOR side = (plane == 0) ? right : up;
			XMVECTOR previous = XMVectorZero();
			for (int i = 0; i <= kSegments; ++i)
			{
				const float angle = (float)i / (float)kSegments * XM_2PI;
				const float c = std::cos(angle);
				const float sn = std::sin(angle);
				// Direction at this angle, and the gain the DSP would give it.
				const XMVECTOR dir = XMVectorAdd(XMVectorScale(fwd, c), XMVectorScale(side, sn));
				const XMVECTOR point = XMVectorAdd(origin,
					XMVectorScale(dir, DirectivityGain(c, weight, power) * scale));
				if (i > 0)
				{
					wi::renderer::RenderableLine line;
					XMStoreFloat3(&line.start, previous);
					XMStoreFloat3(&line.end, point);
					line.color_start = lobeColour;
					line.color_end = lobeColour;
					lines.push_back(line);
				}
				previous = point;
			}
		}

		// The aim axis itself, drawn slightly beyond the lobe so it is readable even when
		// the lobe is a full sphere (weight 0).
		{
			wi::renderer::RenderableLine axis;
			XMStoreFloat3(&axis.start, origin);
			XMStoreFloat3(&axis.end, XMVectorAdd(origin, XMVectorScale(fwd, scale * 1.25f)));
			axis.color_start = XMFLOAT4(1.0f, 0.85f, 0.25f, 1.0f);
			axis.color_end = XMFLOAT4(1.0f, 0.85f, 0.25f, 0.15f);
			lines.push_back(axis);
		}

		// wi::renderer's debug line list is an unlocked global, so it may only be touched
		// from the main thread - and Update runs on a job-system worker by default.
		RunOnMainThread([lines]
		{
			for (const wi::renderer::RenderableLine& line : lines)
				wi::renderer::DrawLine(line);
		});
	}

	void AudioEmitterComponent::OnEnable()
	{
		if (emitter_ && playOnStart)
			emitter_->Play();
	}

	void AudioEmitterComponent::OnDisable()
	{
		// A disabled speaker is silent but keeps its object, so re-enabling does not
		// pay for a new Steam Audio source.
		if (emitter_)
			emitter_->Pause();
	}

	void AudioEmitterComponent::Destroy()
	{
		if (emitter_)
		{
			audio::AudioEngine::Get().Destroy(emitter_);
			emitter_.reset();
		}
	}

	void AudioEmitterComponent::DescribeParams(wi::vector<NativeParam>& out)
	{
		// The same names Start() binds, so what the inspector writes is what Bind()
		// reads back on the next load. Group labels are compared by POINTER by the
		// renderer, so every row in a section has to use the SAME literal - hence the
		// constants rather than repeating the string.
		static const char* kTransport = "Transport";
		static const char* kSource = "Source";
		static const char* kMix = "Mix";
		static const char* kDistance = "3D: distance and air";
		static const char* kDirectivity = "3D: directivity";
		static const char* kOcclusion = "3D: occlusion and transmission";
		static const char* kReflections = "3D: reflections and pathing";
		static const char* kRender = "3D: rendering";

		// transport
		// Live controls, not settings: none of these are written to metadata, because
		// where the playhead is and whether it is moving are not scene data. The lambdas
		// are capture-less so NativeParam stays a plain struct; each casts the base
		// reference back to this type, which is safe because only this component hands
		// these particular function pointers out.
		out.push_back(NativeParam::Action("Play",
			[](wi::scene::NativeComponent& c) { static_cast<AudioEmitterComponent&>(c).Play(); },
			"Start, or resume from where Pause left off.", kTransport));
		out.push_back(NativeParam::Action("Pause",
			[](wi::scene::NativeComponent& c) { static_cast<AudioEmitterComponent&>(c).Pause(); },
			"Hold position. Play resumes from here.", kTransport, /*sameLine*/ true));
		out.push_back(NativeParam::Action("Stop",
			[](wi::scene::NativeComponent& c) { static_cast<AudioEmitterComponent&>(c).Stop(); },
			"Stop and rewind to the start of the play region.", kTransport, /*sameLine*/ true));

		// Seek. Reads the emitter's real playhead every frame, so it tracks playback,
		// and dragging it seeks. The upper bound is the clip's length, queried live
		// because the clip can be swapped underneath.
		out.push_back(NativeParam::Live("seek",
			[](wi::scene::NativeComponent& c) -> float {
				const auto& e = static_cast<AudioEmitterComponent&>(c).GetEmitter();
				return e ? e->GetTime() : 0.0f;
			},
			[](wi::scene::NativeComponent& c, float v) {
				const auto& e = static_cast<AudioEmitterComponent&>(c).GetEmitter();
				if (e) e->SetTime(v);
			},
			0.0f, 1.0f,
			[](wi::scene::NativeComponent& c) -> float {
				const auto& e = static_cast<AudioEmitterComponent&>(c).GetEmitter();
				if (!e) return 1.0f;
				const audio::AudioClip clip = e->GetClip();
				return clip ? clip->GetLengthSeconds() : 1.0f;
			},
			"Playback position in seconds. Drag to scrub.", kTransport));

		// State readout, and a level meter fed by the emitter's own output tap - the
		// same buffer a recorder or a lip-sync envelope would read. It is the quickest
		// answer to "is this thing actually making sound", which a Play button alone
		// never tells you.
		out.push_back(NativeParam::Readout("state",
			[](wi::scene::NativeComponent& c) -> float {
				auto& self = static_cast<AudioEmitterComponent&>(c);
				return self.IsPlaying() ? 1.0f : 0.0f;
			},
			"%.0f", false, "1 while playing, 0 when stopped or paused.", kTransport));
		out.push_back(NativeParam::Readout("level",
			[](wi::scene::NativeComponent& c) -> float {
				const auto& e = static_cast<AudioEmitterComponent&>(c).GetEmitter();
				return e ? e->Output().GetRMS() : 0.0f;
			},
			nullptr, /*asBar*/ true,
			"RMS of what this emitter radiated, post-volume and pre-spatialization.", kTransport));

		out.push_back(NativeParam::Asset("clip", &clip,
			"Drag a .wav or .ogg here from the Resource Explorer, or type a path. "
			"Leave empty to push samples into the emitter's Input() buffer instead.", kSource));
		out.push_back(NativeParam::Bool("playOnStart", &playOnStart, nullptr, kSource));
		out.push_back(NativeParam::Bool("loop", &loop, nullptr, kSource));
		out.push_back(NativeParam::Float("startDelay", &startDelay, 0.0f, 10.0f,
			"Seconds to wait before the first playback.", kSource));

		out.push_back(NativeParam::Float("volume", &volume, 0.0f, 1.0f, nullptr, kMix));
		out.push_back(NativeParam::Float("pitch", &pitch, 0.1f, 4.0f,
			"Resamples on the fly, so it shifts pitch AND speed.", kMix));
		out.push_back(NativeParam::Enum("submix", &submix,
			"SoundEffect\0Music\0Voice\0UI\0Ambient\0",
			"Volume group this emitter belongs to.", kMix));
		out.push_back(NativeParam::Bool("spatial", &spatial,
			"Off routes through OpenAL's 2D path and skips Steam Audio entirely.", kMix));
		out.push_back(NativeParam::Bool("followTransform", &followTransform,
			"Track this entity's TransformComponent every frame.", kMix));
		out.push_back(NativeParam::Bool("computeVelocity", &computeVelocity,
			"Derive velocity from the frame's movement, for Doppler.", kMix));

		out.push_back(NativeParam::Enum("distanceModel", &distanceModel,
			"None\0Inverse distance\0", nullptr, kDistance));
		out.push_back(NativeParam::Float("minDistance", &minDistance, 0.01f, 50.0f,
			"Below this the sound stops getting louder.", kDistance));
		out.push_back(NativeParam::Float("maxDistance", &maxDistance, 1.0f, 2000.0f,
			"Past this the emitter is culled and not mixed at all.", kDistance));
		out.push_back(NativeParam::Enum("airAbsorption", &airAbsorption,
			"None\0Default\0Exponential\0",
			"High frequencies lost over distance.", kDistance));
		out.push_back(NativeParam::Float("airAbsorptionLow", &airAbsorptionLow, 0.0f, 0.01f, nullptr, kDistance));
		out.push_back(NativeParam::Float("airAbsorptionMid", &airAbsorptionMid, 0.0f, 0.01f, nullptr, kDistance));
		out.push_back(NativeParam::Float("airAbsorptionHigh", &airAbsorptionHigh, 0.0f, 0.10f, nullptr, kDistance));

		out.push_back(NativeParam::Enum("forwardAxis", &forwardAxis,
			"+Z\0-Z\0-Y\0+Y\0+X\0-X\0",
			"Which local axis the speaker points down. Same numbering as sticRay and the "
			"projector, so everything on one entity agrees about forward.", kDirectivity));
		out.push_back(NativeParam::Bool("applyDirectivity", &applyDirectivity,
			"Make the source face somewhere instead of radiating equally. Implied by any "
			"non-zero dipoleWeight.", kDirectivity));
		out.push_back(NativeParam::Float("dipoleWeight", &dipoleWeight, 0.0f, 1.0f,
			"0 omnidirectional, 1 figure-of-eight along the entity's forward axis.", kDirectivity));
		out.push_back(NativeParam::Float("dipolePower", &dipolePower, 0.0f, 8.0f,
			"Lobe sharpness: 1 is a soft cardioid, 8 is a searchlight.", kDirectivity));
		out.push_back(NativeParam::Bool("debugDraw", &debugDraw,
			"Draw the directivity lobe in the world: green while radiating, grey when "
			"silent, with a yellow line down the aim axis.", kDirectivity));
		out.push_back(NativeParam::Float("debugScale", &debugScale, 0.1f, 50.0f,
			"Metres the lobe is drawn at full gain. Purely visual.", kDirectivity));

		out.push_back(NativeParam::Enum("occlusion", &occlusion,
			"Off\0Raycast\0Volumetric\0",
			"Raycast is binary; volumetric samples a sphere for partial occlusion.", kOcclusion));
		out.push_back(NativeParam::Float("occlusionRadius", &occlusionRadius, 0.0f, 20.0f,
			"Volumetric only: the source's apparent size.", kOcclusion));
		out.push_back(NativeParam::Int("occlusionSamples", &occlusionSamples, 1, 128,
			"Volumetric only: rays per update.", kOcclusion));
		out.push_back(NativeParam::Enum("transmission", &transmission,
			"Off\0Frequency independent\0Frequency dependent\0",
			"What gets through the wall when the path is blocked.", kOcclusion));
		out.push_back(NativeParam::Int("transmissionRays", &transmissionRays, 1, 64, nullptr, kOcclusion));
		out.push_back(NativeParam::Bool("applyOcclusion", &applyOcclusion, nullptr, kOcclusion));
		out.push_back(NativeParam::Bool("applyTransmission", &applyTransmission, nullptr, kOcclusion));

		out.push_back(NativeParam::Bool("reflections", &reflections,
			"Geometry-driven reverb. Needs scene geometry registered with the spatializer.", kReflections));
		out.push_back(NativeParam::Float("reflectionsMix", &reflectionsMix, 0.0f, 1.0f, nullptr, kReflections));
		out.push_back(NativeParam::Bool("pathing", &pathing,
			"Sound going around a corner instead of through the wall.", kReflections));
		out.push_back(NativeParam::Int("pathingVisSamples", &pathingVisSamples, 1, 32, nullptr, kReflections));
		out.push_back(NativeParam::Float("pathingVisRadius", &pathingVisRadius, 0.0f, 10.0f, nullptr, kReflections));
		out.push_back(NativeParam::Float("pathingVisThreshold", &pathingVisThreshold, 0.0f, 1.0f, nullptr, kReflections));
		out.push_back(NativeParam::Float("pathingRange", &pathingRange, 1.0f, 5000.0f, nullptr, kReflections));
		out.push_back(NativeParam::Bool("pathingValidation", &pathingValidation, nullptr, kReflections));

		out.push_back(NativeParam::Enum("output", &output,
			"Binaural (HRTF)\0Panning\0Ambisonics\0", nullptr, kRender));
		out.push_back(NativeParam::Enum("interpolation", &interpolation,
			"Nearest\0Bilinear\0",
			"Bilinear for anything that moves; nearest is cheaper but steps audibly.", kRender));
		out.push_back(NativeParam::Float("spatialBlend", &spatialBlend, 0.0f, 1.0f,
			"1 fully spatialized, 0 flat in the head.", kRender));
		out.push_back(NativeParam::Bool("applyDistanceAttenuation", &applyDistanceAttenuation, nullptr, kRender));
		out.push_back(NativeParam::Bool("applyAirAbsorption", &applyAirAbsorption, nullptr, kRender));
	}

	// AudioCollectorComponent

	void AudioCollectorComponent::Start()
	{
		Bind(priority, "priority");
		Bind(volume, "volume");
		Bind(routeToOutput, "routeToOutput");
		Bind(followTransform, "followTransform");
		Bind(output, "output");
		Bind(interpolation, "interpolation");
		Bind(normalization, "normalization");
		Bind(ambisonicsOrder, "ambisonicsOrder");
		Bind(binauralReverb, "binauralReverb");
		Bind(hrtfVolumeGain, "hrtfVolumeGain");
		Bind(sofaFile, "sofaFile");

		audio::AudioEngine& engine = audio::AudioEngine::Get();
		if (!engine.IsInitialized())
		{
			wilog_warning("stAudioCollector: the audio engine is not running - collector on entity %llu is inert.",
				(unsigned long long)entity);
			return;
		}

		// The renderer's channel count is fixed when the collector is built, so the
		// spatial settings have to be right before CreateCollector rather than after.
		collector_ = engine.CreateCollector(componentName + "#" + std::to_string(localID));
		SyncSettings();

		audio::SpatialTransform t;
		if (ReadTransform(GetComponent<TransformComponent>(), t))
			collector_->SetTransform(t);
	}

	void AudioCollectorComponent::SyncSettings()
	{
		if (!collector_)
			return;

		collector_->SetVolume(volume);
		collector_->SetPriority(priority);
		collector_->SetRouteToOutput(routeToOutput);

		audio::CollectorSpatialSettings& s = collector_->SpatialSettings();
		s.output = ToEnum(output, audio::SpatialOutput::Binaural, 3);
		s.interpolation = ToEnum(interpolation, audio::HRTFInterpolation::Bilinear, 2);
		s.normalization = ToEnum(normalization, audio::HRTFNormalization::None, 2);
		s.ambisonicsOrder = std::clamp(ambisonicsOrder, 0, 3);
		s.binauralReverb = binauralReverb;
		s.hrtfVolumeGain = hrtfVolumeGain;
		s.sofaFile = sofaFile;
		collector_->ApplySpatialSettings();
	}

	void AudioCollectorComponent::Update(float dt)
	{
		if (!collector_)
			return;
		SyncSettings();
		if (!followTransform)
			return;
		audio::SpatialTransform t;
		if (ReadTransform(GetComponent<TransformComponent>(), t))
			collector_->SetTransform(t);
		(void)dt;
	}

	void AudioCollectorComponent::OnEnable()
	{
		if (collector_) collector_->SetEnabled(true);
	}

	void AudioCollectorComponent::OnDisable()
	{
		if (collector_) collector_->SetEnabled(false);
	}

	void AudioCollectorComponent::Destroy()
	{
		if (collector_)
		{
			audio::AudioEngine::Get().Destroy(collector_);
			collector_.reset();
		}
	}

	void AudioCollectorComponent::DescribeParams(wi::vector<NativeParam>& out)
	{
		static const char* kRouting = "Routing";
		static const char* kListener = "3D: listener";

		out.push_back(NativeParam::Int("priority", &priority, -100, 100,
			"Highest enabled collector becomes the player's ears and the simulator's point of view.", kRouting));
		out.push_back(NativeParam::Float("volume", &volume, 0.0f, 1.0f, nullptr, kRouting));
		out.push_back(NativeParam::Bool("routeToOutput", &routeToOutput,
			"Off still renders into this collector's own buffer - a microphone recording a room nobody is in.", kRouting));
		out.push_back(NativeParam::Bool("followTransform", &followTransform, nullptr, kRouting));

		out.push_back(NativeParam::Enum("output", &output,
			"Binaural (HRTF)\0Panning\0Ambisonics\0", nullptr, kListener));
		out.push_back(NativeParam::Enum("interpolation", &interpolation,
			"Nearest\0Bilinear\0", nullptr, kListener));
		out.push_back(NativeParam::Enum("normalization", &normalization,
			"None\0RMS\0",
			"RMS level-matches the HRIRs so turning your head does not change loudness.", kListener));
		out.push_back(NativeParam::Int("ambisonicsOrder", &ambisonicsOrder, 0, 3,
			"Ambisonics output only: 1 = 4 channels, 2 = 9, 3 = 16.", kListener));
		out.push_back(NativeParam::Bool("binauralReverb", &binauralReverb,
			"Decode the reflection bus through the HRTF too.", kListener));
		out.push_back(NativeParam::Float("hrtfVolumeGain", &hrtfVolumeGain, -20.0f, 20.0f,
			"dB applied to the HRTF itself.", kListener));
		out.push_back(NativeParam::Asset("sofaFile", &sofaFile,
			"Custom HRTF (.sofa). Empty uses Steam Audio's built-in.", kListener));
	}
}

// registration
// Explicit, not ST_REGISTER_NATIVE_COMPONENT, and called from wi::audio::InitializeOpenAL.
//
// The macro registers from a static initializer, which works for the framework's
// components (Framework/ is compiled straight into each game executable) but NOT for
// one living here. Engine/ builds into the Simtary_common STATIC LIBRARY, and a linker
// pulls an object file out of an archive only when something already being linked
// references a symbol in it. Nothing referenced this translation unit, so the whole
// object was dropped, its static initializer never ran, and the components were absent
// from the editor's "Project components" list while sticRay and friends showed up fine.
//
// RegisterAudioComponents() is that missing reference: it is declared in the header and
// called during engine start-up, which forces the object in and registers the names in
// a defined order rather than at unspecified static-init time.
namespace st
{
	void RegisterAudioComponents()
	{
		using namespace wi::scene;

		// Idempotent: registering the same name twice would leave a duplicate in the
		// editor's Add Component list.
		static bool registered = false;
		if (registered)
			return;
		registered = true;

		RegisterNativeComponent("stAudioEmitter",
			[] { return std::unique_ptr<NativeComponent>(new AudioEmitterComponent()); },
			GetNativeTypeID<AudioEmitterComponent>());
		RegisterNativeComponent("stAudioCollector",
			[] { return std::unique_ptr<NativeComponent>(new AudioCollectorComponent()); },
			GetNativeTypeID<AudioCollectorComponent>());

		// Speaker/Microphone are the vocabulary these components were asked for. An
		// alias costs one entry in the registry and makes both spellings work in the
		// editor's Add Component list and in an already-saved scene.
		RegisterNativeComponent("stSpeaker",
			[] { return std::unique_ptr<NativeComponent>(new AudioEmitterComponent()); },
			GetNativeTypeID<AudioEmitterComponent>());
		RegisterNativeComponent("stMicrophone",
			[] { return std::unique_ptr<NativeComponent>(new AudioCollectorComponent()); },
			GetNativeTypeID<AudioCollectorComponent>());
	}
}
