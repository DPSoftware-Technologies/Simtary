#pragma once
// ─── stAudioSpatial: the Steam Audio layer ───────────────────────────────────────
//
// Steam Audio owns every 3D decision in this engine: distance attenuation, air
// absorption, directivity, occlusion, transmission, geometry-driven reflections and
// pathing, and the HRTF itself. OpenAL owns none of them - it is the output device
// and the 2D mixer, nothing more. Splitting it any other way means two systems pan
// and attenuate the same sound twice.
//
// phonon.h is deliberately NOT included here. Everything the SDK exposes is mirrored
// as engine enums and plain structs so components, the editor and game code can talk
// about "volumetric occlusion with 32 samples" without the SDK being on the include
// path. Only stAudioSpatial.cpp sees phonon.h.
//
// When SIMTARY_HAS_STEAMAUDIO is not defined the whole file still compiles and the
// implementation degrades to distance attenuation plus equal-power panning. Sound
// keeps working, it just stops being convincing - that is the deal so a machine
// without the SDK fetched can still build and run the engine.
//
// Coordinate space: Steam Audio is right-handed, -Z forward, +Y up, metres - the
// same convention the engine's transforms already use, so vectors pass straight
// through. Positions handed in are RENDER-ORIGIN-RELATIVE floats (see
// TransformComponent's large-world extension), never absolute world doubles: what
// matters to a spatializer is the emitter-to-listener vector, and rebasing both ends
// around the same origin keeps that exact while staying in float.

#include "CommonInclude.h"
// XMFLOAT3 and the DirectXMath using-declarations. CommonInclude.h is deliberately
// minimal and does NOT bring them in, so this header must ask for them itself or it
// only compiles inside a translation unit that happened to include wiMath first.
#include "wiMath.h"

#include <memory>
#include <string>
#include <cstdint>

namespace st::audio
{
	// ── enums mirroring the SDK ─────────────────────────────────────────────────

	enum class DistanceAttenuationModel
	{
		None,             // no attenuation at all - a sound that is equally loud everywhere
		InverseDistance,  // Steam Audio's 1/max(d, minDistance)
		Default = InverseDistance,
	};

	enum class AirAbsorptionModel
	{
		None,
		Default,      // Steam Audio's built-in 3-band atmospheric curve
		Exponential,  // exp(-coefficient[band] * distance) with your own coefficients
	};

	enum class OcclusionMode
	{
		Off,
		Raycast,     // one ray, listener to source: binary occluded / not
		Volumetric,  // a sphere of `radius` sampled with `numSamples` rays: partial occlusion
	};

	enum class TransmissionMode
	{
		Off,
		FrequencyIndependent,  // one attenuation value for the whole spectrum
		FrequencyDependent,    // per-band, from the occluding material
	};

	enum class ReflectionMode
	{
		Off,
		Convolution,     // full impulse response: the most accurate, the most expensive
		Parametric,      // fits a decay curve, feeds a reverb - cheap, no early reflections
		Hybrid,          // convolution for early reflections, parametric for the tail
		TrueAudioNext,   // AMD TAN GPU convolution (needs a TAN-capable device)
	};

	enum class HRTFInterpolation
	{
		Nearest,   // snap to the closest measured HRIR - cheapest, audibly steppy on movement
		Bilinear,  // blend the four neighbours - what you want for anything that moves
	};

	enum class HRTFNormalization
	{
		None,
		RMS,  // level-match the HRIRs so turning your head does not change loudness
	};

	enum class SceneBackend
	{
		Default,     // Steam Audio's own built-in BVH
		Embree,      // Intel Embree CPU ray tracer (faster on big static scenes)
		RadeonRays,  // AMD GPU ray tracer
	};

	enum class SpatialOutput
	{
		Binaural,     // HRTF stereo - headphones
		Panning,      // plain amplitude panning to the speaker layout - no HRTF colouring
		Ambisonics,   // B-format at `ambisonicsOrder`, decoded by the collector
	};

	// ── per-emitter settings ────────────────────────────────────────────────────
	// Everything Steam Audio lets you set per source. Defaults are the SDK's own
	// defaults, so a component that touches nothing behaves like a stock IPLSource.
	struct EmitterSpatialSettings
	{
		// --- direct path ---
		DistanceAttenuationModel distanceModel = DistanceAttenuationModel::Default;
		float minDistance = 1.0f;          // below this, attenuation stops increasing
		float maxDistance = 500.0f;        // engine-side cull: past this the emitter is not mixed
		AirAbsorptionModel airAbsorption = AirAbsorptionModel::None;
		float airAbsorptionCoefficients[3] = { 0.0002f, 0.0017f, 0.0182f }; // low / mid / high, Exponential only

		// --- directivity (a speaker that faces somewhere) ---
		// 0 = omnidirectional, 1 = pure dipole (figure of eight). `dipolePower`
		// sharpens the lobe: 1 is a soft cardioid-ish shape, 8 is a searchlight.
		float dipoleWeight = 0.0f;
		float dipolePower = 1.0f;

		// --- occlusion / transmission ---
		OcclusionMode occlusion = OcclusionMode::Off;
		float occlusionRadius = 1.0f;      // Volumetric only: the source's apparent size
		int   occlusionSamples = 16;       // Volumetric only: rays per update
		TransmissionMode transmission = TransmissionMode::Off;
		int   transmissionRays = 8;        // rays used to find what material is in the way

		// --- reflections ---
		bool  reflections = false;
		float reflectionsMix = 1.0f;       // gain on the reflected signal, 0..1

		// --- pathing (sound going around a corner instead of through the wall) ---
		bool  pathing = false;
		int   pathingVisSamples = 4;       // rays per probe-pair visibility test
		float pathingVisRadius = 1.0f;     // probe visibility ray thickness
		float pathingVisThreshold = 0.1f;  // fraction of rays that must pass to call it visible
		float pathingRange = 1000.0f;      // longest path considered, metres
		bool  pathingValidation = true;    // re-check baked paths against the live scene

		// --- rendering ---
		SpatialOutput output = SpatialOutput::Binaural;
		HRTFInterpolation interpolation = HRTFInterpolation::Bilinear;
		// 0 = fully spatialized, 1 = fully "in your head" stereo. Unity's spatialBlend
		// inverted: this is Steam Audio's own convention, where 1.0 IS spatialized.
		float spatialBlend = 1.0f;
		bool  applyDistanceAttenuation = true;
		bool  applyAirAbsorption = true;
		bool  applyDirectivity = false;
		bool  applyOcclusion = true;
		bool  applyTransmission = true;
	};

	// ── per-collector settings ──────────────────────────────────────────────────
	struct CollectorSpatialSettings
	{
		SpatialOutput output = SpatialOutput::Binaural;
		HRTFInterpolation interpolation = HRTFInterpolation::Bilinear;
		HRTFNormalization normalization = HRTFNormalization::None;
		int  ambisonicsOrder = 1;        // 1 = 4 channels, 2 = 9, 3 = 16
		bool binauralReverb = true;      // decode the reflection bus through the HRTF too
		float hrtfVolumeGain = 0.0f;     // dB applied to the HRTF itself
		std::string sofaFile;            // optional custom HRTF (.sofa); empty = built-in
	};

	// ── global simulation settings ──────────────────────────────────────────────
	// These size the ray tracer and cannot change without rebuilding the simulator,
	// so they are read once at Initialize().
	struct SimulationSettings
	{
		SceneBackend backend = SceneBackend::Default;
		int   maxRays = 4096;         // rays cast per reflection update
		int   raysPerBounce = 512;    // batch size within one update
		int   maxBounces = 4;
		float maxDuration = 2.0f;     // longest impulse response, seconds
		int   maxOrder = 1;           // ambisonic order of the reflection bus
		int   maxSources = 64;        // sources the simulator can track at once
		int   threads = 2;            // ray-tracing worker threads
		float irradianceMinDistance = 1.0f;
		bool  reflections = true;     // build the reflection simulator at all
		bool  pathing = true;         // build the pathing simulator at all
		float updateRateHz = 10.0f;   // how often the reflection/pathing job re-runs
	};

	// ── live per-frame state pushed from the game thread ────────────────────────
	// Plain data, snapshotted under a lock once per frame and read lock-free by the
	// audio thread. Positions and vectors are render-origin-relative (see the header
	// comment); `velocity` is metres/second and only matters if Doppler is on.
	struct SpatialTransform
	{
		XMFLOAT3 position = XMFLOAT3(0, 0, 0);
		XMFLOAT3 forward = XMFLOAT3(0, 0, 1);
		XMFLOAT3 up = XMFLOAT3(0, 1, 0);
		XMFLOAT3 velocity = XMFLOAT3(0, 0, 0);
	};

	// What the simulator worked out about one emitter/collector pair. Readable from
	// game code for gameplay decisions ("can the guard hear me?").
	struct SpatialResult
	{
		float distance = 0.0f;
		float distanceAttenuation = 1.0f;
		float occlusion = 1.0f;      // 1 = clear line of sight, 0 = fully blocked
		float transmission[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // per-band, blocked path only
		float directivity = 1.0f;
		float airAbsorption[3] = { 1.0f, 1.0f, 1.0f };
		bool  audible = false;       // survived the maxDistance cull and has signal
	};

	// ── the spatializer ─────────────────────────────────────────────────────────
	// One per process. Owned by the audio engine; Initialize/Shutdown are called from
	// stAudioEngine and are not for game code.
	class Spatializer
	{
	public:
		static Spatializer& Get();

		bool Initialize(int sampleRate, int frameSize, const SimulationSettings& settings);
		void Shutdown();
		bool IsInitialized() const;
		// True when a real Steam Audio context is behind this - false means the build
		// has no SDK and the fallback panner is running.
		bool IsSteamAudioAvailable() const;
		const char* GetBackendName() const;

		const SimulationSettings& GetSimulationSettings() const;

		// Geometry. Meshes are handed over as triangle soup with per-triangle material
		// indices; the engine's static scene is committed once, then only moving
		// geometry needs re-committing. Absent any geometry, occlusion and reflections
		// are simply inert - the direct path still works.
		struct MaterialDesc
		{
			float absorption[3] = { 0.10f, 0.20f, 0.30f }; // per-band, 0 = reflective, 1 = dead
			float scattering = 0.05f;                      // 0 = mirror, 1 = fully diffuse
			float transmission[3] = { 0.20f, 0.05f, 0.01f };// per-band pass-through
		};
		// Returns a handle (>=0) to be used as the material index of subsequent
		// meshes, or -1 if the spatializer is not running.
		int  AddMaterial(const MaterialDesc& material);

		// Add a static mesh. `vertices` is xyz triples, `indices` is triangle triples,
		// `materialIndices` is one entry per TRIANGLE. Returns a mesh handle, or 0 on
		// failure. Transform is baked in: move a mesh by removing and re-adding it.
		uint32_t AddStaticMesh(const float* vertices, int vertexCount,
			const int32_t* indices, int triangleCount,
			const int32_t* materialIndices);
		void RemoveMesh(uint32_t meshHandle);
		void ClearGeometry();
		// Rebuild the acceleration structure. Cheap when nothing changed; call it once
		// after a batch of AddStaticMesh, not per mesh.
		void CommitScene();

		// Run one reflection/pathing pass. Called by the engine's simulation job at
		// SimulationSettings::updateRateHz, never from the audio thread.
		void RunSimulation();

	private:
		Spatializer();
		~Spatializer();
		Spatializer(const Spatializer&) = delete;
		Spatializer& operator=(const Spatializer&) = delete;

		struct Impl;
		std::unique_ptr<Impl> impl_;
		friend class SpatialSource;
		friend class SpatialRenderer;
	};

	// One emitter's presence in the simulator: an IPLSource plus the direct-path
	// effect state that belongs to the source rather than to a listener.
	class SpatialSource
	{
	public:
		SpatialSource();
		~SpatialSource();
		SpatialSource(const SpatialSource&) = delete;
		SpatialSource& operator=(const SpatialSource&) = delete;

		bool Create(const EmitterSpatialSettings& settings);
		void Destroy();
		bool IsValid() const;

		// Push this frame's transform and settings. Game thread, once per frame.
		void SetTransform(const SpatialTransform& transform);
		void SetSettings(const EmitterSpatialSettings& settings);
		const EmitterSpatialSettings& GetSettings() const { return settings_; }

		// The simulator's latest verdict for this source against the active listener.
		SpatialResult GetResult() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
		EmitterSpatialSettings settings_;
		friend class SpatialRenderer;
	};

	// One collector's rendering state. Effect objects hold interpolation history, and
	// that history belongs to the (emitter, collector) PAIR - two microphones hearing
	// the same emitter are two different filters - so the per-emitter effects live
	// here, keyed by source, rather than on the emitter.
	class SpatialRenderer
	{
	public:
		SpatialRenderer();
		~SpatialRenderer();
		SpatialRenderer(const SpatialRenderer&) = delete;
		SpatialRenderer& operator=(const SpatialRenderer&) = delete;

		bool Create(const CollectorSpatialSettings& settings);
		void Destroy();
		bool IsValid() const;

		void SetSettings(const CollectorSpatialSettings& settings);
		void SetTransform(const SpatialTransform& transform);
		const SpatialTransform& GetTransform() const { return transform_; }

		// Number of channels Render() writes: 2 for Binaural/Panning, (order+1)^2 for
		// Ambisonics.
		int GetOutputChannels() const;

		// Begin an audio block: clears the accumulation bus. Audio thread.
		void BeginBlock(int frames);
		// Spatialize one emitter's mono input into the bus. Audio thread. `gain` is
		// the emitter's own volume, applied before the spatial chain.
		void Accumulate(SpatialSource& source, const float* monoInput, int frames, float gain);
		// Finish the block: decodes ambisonics if needed and writes GetOutputChannels()
		// planes into `out`. Audio thread.
		void EndBlock(float* const* out, int frames);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
		CollectorSpatialSettings settings_;
		SpatialTransform transform_;
	};
}
