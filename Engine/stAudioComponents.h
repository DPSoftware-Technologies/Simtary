#pragma once
// ─── stAudioComponents: audio on a GameObject ────────────────────────────────────
//
// Two native components, and every entity can carry either or both:
//
//	"stAudioEmitter"    a SPEAKER. Owns an st::audio::Emitter, follows the entity's
//	                    TransformComponent every frame, and exposes the whole Steam
//	                    Audio source option set as metadata arguments.
//	"stAudioCollector"  a MICROPHONE. Owns an st::audio::Collector, follows the same
//	                    transform, and renders the scene from where the entity is.
//	                    The highest-priority enabled one is the player's ears; the
//	                    rest keep rendering into their own buffers.
//
// Both are attachable from the Wicked Editor with no code, exactly like any other
// native component:
//
//	NCI_0                     = "stAudioEmitter"
//	NCA_0_clip                = "assets/contents/audio/engine_idle.ogg"
//	NCA_0_playOnStart         = true
//	NCA_0_loop                = true
//	NCA_0_volume              = 0.8
//	NCA_0_minDistance         = 2.0
//	NCA_0_maxDistance         = 300.0
//	NCA_0_occlusion           = 2        (0 off, 1 raycast, 2 volumetric)
//	NCA_0_occlusionRadius     = 0.5
//	NCA_0_reflections         = true
//	NCA_0_dipoleWeight        = 0.7      (a horn pointing down the entity's forward axis)
//
//	NCI_1                     = "stAudioCollector"
//	NCA_1_priority            = 100
//	NCA_1_output              = 0        (0 binaural, 1 panning, 2 ambisonics)
//
// or from C++ on the same entity:
//
//	if (auto* speaker = GetComponent<st::AudioEmitterComponent>()) {
//	    speaker->GetEmitter()->SetPitch(1.0f + rpm * 0.0002f);
//	    float level = speaker->GetEmitter()->Output().GetRMS();
//	}
//
// Both run their per-frame work on the job system's worker threads: pushing a
// transform and a volume into the audio engine touches nothing the scene owns.

#include "stNativeComponent.h"
#include "stAudioEngine.h"

#include <string>

namespace st
{
	// Register stAudioEmitter / stAudioCollector (and the stSpeaker / stMicrophone
	// aliases) with the native component registry.
	//
	// Called from wi::audio::InitializeOpenAL during engine start-up. It has to be an
	// explicit call rather than ST_REGISTER_NATIVE_COMPONENT's static initializer:
	// Engine/ is a static library, so a translation unit nothing references is dropped
	// by the linker along with its static initializers, and these components would
	// silently never appear in the editor. Idempotent.
	void RegisterAudioComponents();

	// ── Emitter (Speaker) ───────────────────────────────────────────────────────
	struct AudioEmitterComponent : wi::scene::NativeComponent
	{
		// ── asset / transport ───────────────────────────────────────────────────
		std::string clip;                 // path; empty means "I will push samples into Input()"
		bool  playOnStart = true;
		bool  loop = false;
		float volume = 1.0f;
		float pitch = 1.0f;
		float startDelay = 0.0f;
		int   submix = (int)audio::Submix::SoundEffect;
		// false routes to OpenAL's 2D path and skips Steam Audio entirely.
		bool  spatial = true;
		// Follow the entity's transform. Turn it off for an emitter positioned by hand.
		bool  followTransform = true;
		// Feed Steam Audio a velocity derived from how far the entity moved this frame.
		bool  computeVelocity = true;

		// ── Steam Audio: direct path ────────────────────────────────────────────
		int   distanceModel = (int)audio::DistanceAttenuationModel::Default; // 0 none, 1 inverse
		float minDistance = 1.0f;
		float maxDistance = 500.0f;
		int   airAbsorption = (int)audio::AirAbsorptionModel::None;   // 0 none, 1 default, 2 exponential
		float airAbsorptionLow = 0.0002f;
		float airAbsorptionMid = 0.0017f;
		float airAbsorptionHigh = 0.0182f;

		// ── Steam Audio: directivity ────────────────────────────────────────────
		// Which of the entity's local axes the speaker points down. Same numbering as
		// st::LocalAxes (Framework/scene/Ray.h) so a speaker, a laser and a projector on
		// one entity all agree about "forward": 0 +Z, 1 -Z, 2 -Y, 3 +Y, 4 +X, 5 -X.
		// Engine/ cannot include Framework/, so the table is duplicated in the .cpp - it
		// is append-only there for the same reason it is there: the numbers are stored in
		// scene metadata and renumbering would silently re-aim everything already placed.
		int   forwardAxis = 0;
		// 0 = omnidirectional (radiates equally everywhere), 1 = pure dipole, a
		// figure-of-eight along forwardAxis. Anything above 0 makes the speaker
		// DIRECTIONAL and implies applyDirectivity.
		float dipoleWeight = 0.0f;
		// Lobe sharpness. 1 is a soft cardioid-ish shape, 8 is a searchlight.
		float dipolePower = 1.0f;

		// ── debug overlay ───────────────────────────────────────────────────────
		// Draw the directivity lobe in the world, so where a speaker is actually
		// pointing is visible rather than inferred from two numbers.
		bool  debugDraw = false;
		float debugScale = 3.0f;      // metres the lobe is drawn at full gain

		// ── Steam Audio: occlusion / transmission ───────────────────────────────
		int   occlusion = (int)audio::OcclusionMode::Off;        // 0 off, 1 raycast, 2 volumetric
		float occlusionRadius = 1.0f;
		int   occlusionSamples = 16;
		int   transmission = (int)audio::TransmissionMode::Off;  // 0 off, 1 freq-independent, 2 freq-dependent
		int   transmissionRays = 8;

		// ── Steam Audio: reflections / pathing ──────────────────────────────────
		bool  reflections = false;
		float reflectionsMix = 1.0f;
		bool  pathing = false;
		int   pathingVisSamples = 4;
		float pathingVisRadius = 1.0f;
		float pathingVisThreshold = 0.1f;
		float pathingRange = 1000.0f;
		bool  pathingValidation = true;

		// ── Steam Audio: rendering ──────────────────────────────────────────────
		int   output = (int)audio::SpatialOutput::Binaural;      // 0 binaural, 1 panning, 2 ambisonics
		int   interpolation = (int)audio::HRTFInterpolation::Bilinear; // 0 nearest, 1 bilinear
		float spatialBlend = 1.0f;
		bool  applyDistanceAttenuation = true;
		bool  applyAirAbsorption = true;
		bool  applyDirectivity = false;
		bool  applyOcclusion = true;
		bool  applyTransmission = true;

		// ── access ──────────────────────────────────────────────────────────────
		const audio::EmitterRef& GetEmitter() const { return emitter_; }

		// The samples this emitter is about to radiate. Write mono blocks here for a
		// procedural engine note, a radio feed, a Faust instrument. Writing to it once
		// makes the emitter ignore `clip`.
		audio::AudioBuffer* GetInputBuffer() { return emitter_ ? &emitter_->Input() : nullptr; }
		// What it actually radiated, post-volume, pre-spatialization.
		const audio::AudioBuffer* GetOutputBuffer() const { return emitter_ ? &emitter_->Output() : nullptr; }

		// The simulator's verdict: distance, attenuation, occlusion, transmission.
		audio::SpatialResult GetSpatialResult() const
		{
			return emitter_ ? emitter_->GetSpatialResult() : audio::SpatialResult{};
		}

		void Play() { if (emitter_) emitter_->Play(); }
		void Stop() { if (emitter_) emitter_->Stop(); }
		void Pause() { if (emitter_) emitter_->Pause(); }
		bool IsPlaying() const { return emitter_ && emitter_->IsPlaying(); }

		// Push the component's fields into the engine object. Called automatically
		// every Update; call it by hand after changing a field if you need it to land
		// before the next frame.
		void SyncSettings();

		// Load `clip` if it changed since the last load, and start playing when
		// playOnStart is set. Called every Update, so assigning `clip` at runtime - from
		// the inspector, a drag from the Resource Explorer, or game code - takes effect
		// on the next frame instead of only at Start().
		void SyncClip();

		void Start() override;
		void Update(float dt) override;
		void OnEnable() override;
		void OnDisable() override;
		void Destroy() override;
		// Editor inspector. Engine/ cannot call ImGui, so the fields are described as
		// data and Framework/devui/imnativecomponents.cpp draws them.
		void DescribeParams(wi::vector<NativeParam>& out) override;

	private:
		// Draw the lobe for this frame. Queues onto the main thread internally, so it is
		// safe to call from the parallel Update.
		void DrawDirectivity(const audio::SpatialTransform& transform);

		audio::EmitterRef emitter_;
		// The path SyncClip last acted on. Compared against `clip` to decide whether a
		// reload is due; it is updated even when the load FAILS, so a bad path is
		// reported once instead of retried every frame.
		std::string loadedClip_;
		XMFLOAT3 lastPosition_ = XMFLOAT3(0, 0, 0);
		bool hasLastPosition_ = false;
	};

	// ── Collector (Microphone) ──────────────────────────────────────────────────
	struct AudioCollectorComponent : wi::scene::NativeComponent
	{
		// Highest priority among the enabled collectors becomes the player's ears and
		// the simulator's point of view.
		int   priority = 0;
		float volume = 1.0f;
		// A collector that is not routed still renders into its own buffer - which is
		// exactly what a recording microphone in another room is.
		bool  routeToOutput = true;
		bool  followTransform = true;

		// ── Steam Audio: listener options ───────────────────────────────────────
		int   output = (int)audio::SpatialOutput::Binaural;   // 0 binaural, 1 panning, 2 ambisonics
		int   interpolation = (int)audio::HRTFInterpolation::Bilinear;
		int   normalization = (int)audio::HRTFNormalization::None; // 0 none, 1 RMS
		int   ambisonicsOrder = 1;
		bool  binauralReverb = true;
		float hrtfVolumeGain = 0.0f;
		std::string sofaFile;   // custom HRTF; empty uses Steam Audio's built-in

		// ── access ──────────────────────────────────────────────────────────────
		const audio::CollectorRef& GetCollector() const { return collector_; }

		// What this microphone hears, updated every audio block. Stereo for
		// binaural/panning output, (order+1)^2 channels for ambisonics. Peek it for a
		// level meter, drain it into a file to record, packetize it for a radio.
		const audio::AudioBuffer* GetOutputBuffer() const { return collector_ ? &collector_->Output() : nullptr; }
		audio::AudioBuffer* GetOutputBuffer() { return collector_ ? &collector_->Output() : nullptr; }

		bool IsPrimary() const { return collector_ && collector_->IsPrimary(); }

		void SyncSettings();

		void Start() override;
		void Update(float dt) override;
		void OnEnable() override;
		void OnDisable() override;
		void Destroy() override;
		void DescribeParams(wi::vector<NativeParam>& out) override;

	private:
		audio::CollectorRef collector_;
	};
}
