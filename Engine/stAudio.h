#pragma once
#include <memory>

// stAudio: engine audio entry points and the procedural DSP bridge
//
// The audio system proper lives in stAudioEngine.h (device + 2D via OpenAL Soft,
// 3D via Steam Audio), stAudioSpatial.h (the Steam Audio layer), stAudioBuffer.h
// (the sample interchange) and stAudioComponents.h (stAudioEmitter /
// stAudioCollector on scene entities). This header is the small surface the rest of
// the engine touches: bring-up, shutdown, and the adapter that turns a real-time DSP
// generator into an ordinary emitter.
//
// wi::audio::Initialize() (XAudio2 / FAudio) is deliberately never called. The
// legacy wiAudio.cpp stays linked so its Sound/SoundInstance API keeps resolving,
// but it is inert; everything real goes through st::audio.

namespace st::audio { class Emitter; }

namespace wi::audio
{
	// Engine-level startup, called once from wi::initializer in place of the legacy
	// wi::audio::Initialize(). Opens the OpenAL device, brings up Steam Audio, and
	// starts the mix and simulation threads. Safe to call with no output device
	// present: it logs and leaves the system idle rather than failing the boot.
	void InitializeOpenAL();

	// Tear the audio system down. Called from the application shutdown path; also
	// safe to call when Initialize never succeeded.
	void ShutdownAudio();

	// Per-frame pump: elects the primary collector, retires finished one-shots, and
	// keeps the 2D voice gains in step with the volume sliders. No DSP - the mix runs
	// on its own thread. Called from Application::Update.
	void UpdateAudio(float dt);

	// Abstract real-time audio generator. Fills planar (non-interleaved) float output
	// buffers, one per channel, which matches Faust's `dsp::compute(count, inputs,
	// outputs)` output contract - a project-side adapter wraps an AOT Faust `dsp` as a
	// DSPSource, so the same stream drives any procedural generator.
	class DSPSource
	{
	public:
		virtual ~DSPSource() = default;

		// Number of output channels the source writes (1 = mono, 2 = stereo).
		// A source with more than one output is downmixed to mono when the stream is
		// spatialized - a point in space radiates one signal - and kept as-is when it
		// is not.
		virtual int GetNumOutputs() const = 0;

		// Called once before the first Compute(), carrying the render sample rate.
		virtual void Prepare(int sampleRate) = 0;

		// Fill outputs[ch][0 .. frames) with samples in roughly [-1, 1].
		virtual void Compute(int frames, float** outputs) = 0;
	};

	// Streams a DSPSource into the audio engine.
	//
	// It no longer owns a device. Opening a second OpenAL context alongside the
	// engine's would mean two mixers fighting over one process-wide current context,
	// so the stream now renders on its own worker thread into an ordinary
	// st::audio::Emitter's input buffer. Everything the engine can do to an emitter
	// therefore applies to a Faust instrument too: submix routing, volume, and - via
	// GetEmitter() - full Steam Audio spatialization, so a procedural engine note can
	// be occluded by the hull it is inside.
	class DSPStream
	{
	public:
		DSPStream();
		~DSPStream();
		DSPStream(const DSPStream&) = delete;
		DSPStream& operator=(const DSPStream&) = delete;

		// Start rendering `source` into a new emitter. The source must outlive the
		// stream (or until Stop()). `sampleRate` is a request: the engine's mix rate
		// wins, and that is what Prepare() is told. Returns false if the audio engine
		// is not running or a stream is already going.
		bool Start(DSPSource* source, int sampleRate = 48000);

		// Stop streaming, join the worker thread, and destroy the emitter. Idempotent;
		// also called by the destructor.
		void Stop();

		bool IsRunning() const;

		// Master output gain in [0, 1], applied live on the next buffer cycle.
		void SetGain(float gain01);
		float GetGain() const;

		// The emitter carrying this stream, or null when it is not running. Use it to
		// place the instrument in the world, give it a directivity lobe, or read its
		// output tap:
		//
		//	stream.Start(&myFaustDSP);
		//	auto emitter = stream.GetEmitter();
		//	emitter->SetSpatial(true);
		//	emitter->SpatialSettings().occlusion = st::audio::OcclusionMode::Volumetric;
		//	emitter->ApplySpatialSettings();
		std::shared_ptr<st::audio::Emitter> GetEmitter() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
