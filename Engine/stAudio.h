#pragma once
#include <memory>

// ─── stAudio: engine OpenAL audio subsystem ────────────────────────────────────
// The OpenAL-backed audio path for the engine. Its long-term role is to become
// THE engine audio system, replacing the legacy FAudio backend in wiAudio.cpp
// while keeping the wiAudio.h public API (Sound/SoundInstance/3D/reverb) intact.
//
// Stage 1 (this file today): real-time procedural output. DSPStream renders an
// abstract DSPSource to the default output device via OpenAL Soft on a worker
// thread. It knows nothing about Faust — a project-side adapter wraps an AOT
// Faust `dsp` as a DSPSource — so the same stream drives any procedural
// generator. Future stages add sample playback, 3D/HRTF, and object-based audio
// here, after which wiAudio.cpp + FAudio can be removed.

namespace wi::audio
{
	// Engine-level startup for the OpenAL audio subsystem. Called once from
	// wi::initializer instead of the legacy wi::audio::Initialize() (XAudio2 /
	// FAudio) so the old backend is never brought up. Probes the default output
	// device and logs the active OpenAL renderer; DSPStream still owns its own
	// device/context lifetime per stream. Safe to call when no device exists — it
	// logs a warning and leaves the subsystem idle rather than failing.
	void InitializeOpenAL();

	// Abstract real-time audio generator driven by DSPStream on the audio thread.
	// Fills planar (non-interleaved) float output buffers, one per channel, which
	// matches the Faust `dsp::compute(count, inputs, outputs)` output contract.
	class DSPSource
	{
	public:
		virtual ~DSPSource() = default;

		// Number of output channels the source writes (1 = mono, 2 = stereo).
		// Sources with >2 outputs are downmixed to stereo by the stream, but the
		// source is always handed exactly this many output buffers to fill.
		virtual int GetNumOutputs() const = 0;

		// Called once on the audio thread before the first Compute(), carrying the
		// stream's actual render sample rate in Hz.
		virtual void Prepare(int sampleRate) = 0;

		// Fill outputs[ch][0 .. frames) with samples in roughly [-1, 1].
		// `outputs` has GetNumOutputs() channel pointers.
		virtual void Compute(int frames, float** outputs) = 0;
	};

	// Streams a DSPSource to the default OpenAL output device from a dedicated
	// worker thread using queued 16-bit PCM buffers. All OpenAL usage is confined
	// here; only the C API (AL/al.h, AL/alc.h) is touched, so this compiles fine
	// under the project-wide exceptions-off flags.
	class DSPStream
	{
	public:
		DSPStream();
		~DSPStream();
		DSPStream(const DSPStream&) = delete;
		DSPStream& operator=(const DSPStream&) = delete;

		// Opens OpenAL and starts streaming `source`. The source must outlive the
		// stream (or until Stop()). `sampleRate` is the DSP render rate in Hz.
		// Returns false if the device/context could not be created or a stream is
		// already running.
		bool Start(DSPSource* source, int sampleRate = 48000);

		// Stops streaming, joins the worker thread, and releases the device.
		// Idempotent; also called by the destructor.
		void Stop();

		bool IsRunning() const;

		// Master output gain in [0, 1], applied live on the next buffer cycle.
		void SetGain(float gain01);
		float GetGain() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
