#include "stAudio.h"
#include "wiBacklog.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>

namespace wi::audio
{
	// Streaming parameters. Four buffers of 1024 frames at 48 kHz gives ~85 ms of
	// queued audio — enough to ride over frame-time spikes without underrunning,
	// while staying responsive to Start/Stop and gain changes.
	static constexpr int kNumBuffers     = 4;
	static constexpr int kFramesPerBuffer = 1024;

	static inline float clamp01f(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

	void InitializeOpenAL()
	{
		// Probe the default output device so boot logs show the active audio
		// backend (and surface a missing-device early, instead of at first Play).
		// Opened and closed here; DSPStream reopens its own device when streaming.
		ALCdevice* device = alcOpenDevice(nullptr);
		if (device == nullptr)
		{
			wilog_warning("stAudio: no OpenAL output device - audio disabled.");
			return;
		}

		const ALCchar* name = nullptr;
		if (alcIsExtensionPresent(device, "ALC_ENUMERATE_ALL_EXT") == ALC_TRUE)
			name = alcGetString(device, ALC_ALL_DEVICES_SPECIFIER);
		if (name == nullptr)
			name = alcGetString(device, ALC_DEFAULT_DEVICE_SPECIFIER);

		wilog("stAudio: OpenAL initialized (device: %s).", name ? name : "default");
		alcCloseDevice(device);
	}

	struct DSPStream::Impl
	{
		ALCdevice*  device  = nullptr;
		ALCcontext* context = nullptr;
		ALuint      source  = 0;
		ALuint      buffers[kNumBuffers] = {};

		DSPSource* src = nullptr;
		int   sampleRate  = 48000;
		int   srcChannels = 1;   // channels the source fills
		int   outChannels = 1;   // channels sent to OpenAL (source, clamped to 2)
		ALenum format = AL_FORMAT_MONO16;

		std::thread worker;
		std::atomic<bool>  running{ false };
		std::atomic<float> gain{ 1.0f };

		// Scratch reused every buffer cycle (owned by the worker thread once running).
		std::vector<std::vector<float>> planar; // [srcChannels][frames]
		std::vector<float*>  planarPtrs;         // [srcChannels]
		std::vector<int16_t> interleaved;        // outChannels * frames

		// Render one buffer's worth of audio into `buf` and hand it to OpenAL.
		void FillBuffer(ALuint buf)
		{
			src->Compute(kFramesPerBuffer, planarPtrs.data());

			const float g = gain.load(std::memory_order_relaxed);
			if (outChannels == 1)
			{
				const float* mono = planar[0].data();
				for (int i = 0; i < kFramesPerBuffer; ++i)
					interleaved[i] = (int16_t)(clamp01f(mono[i] * g) * 32767.0f);
			}
			else
			{
				const float* L = planar[0].data();
				const float* R = planar[1].data();
				for (int i = 0; i < kFramesPerBuffer; ++i)
				{
					interleaved[2 * i + 0] = (int16_t)(clamp01f(L[i] * g) * 32767.0f);
					interleaved[2 * i + 1] = (int16_t)(clamp01f(R[i] * g) * 32767.0f);
				}
			}

			const ALsizei bytes = (ALsizei)(interleaved.size() * sizeof(int16_t));
			alBufferData(buf, format, interleaved.data(), bytes, sampleRate);
		}

		void Run()
		{
			// OpenAL's current context is process-wide; it was made current in
			// Start() on the calling thread and remains valid here.
			while (running.load(std::memory_order_acquire))
			{
				ALint processed = 0;
				alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
				while (processed-- > 0)
				{
					ALuint buf = 0;
					alSourceUnqueueBuffers(source, 1, &buf);
					FillBuffer(buf);
					alSourceQueueBuffers(source, 1, &buf);
				}

				// Recover from an underrun: if playback stopped while we still want
				// to run, restart it (queued buffers were exhausted under load).
				ALint state = 0;
				alGetSourcei(source, AL_SOURCE_STATE, &state);
				if (state != AL_PLAYING)
				{
					ALint queued = 0;
					alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
					if (queued > 0)
						alSourcePlay(source);
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
	};

	DSPStream::DSPStream() : impl_(std::make_unique<Impl>()) {}
	DSPStream::~DSPStream() { Stop(); }

	bool DSPStream::Start(DSPSource* source, int sampleRate)
	{
		Impl& s = *impl_;
		if (s.running.load() || source == nullptr)
			return false;

		s.src = source;
		s.sampleRate = sampleRate;
		s.srcChannels = source->GetNumOutputs();
		if (s.srcChannels < 1) s.srcChannels = 1;
		s.outChannels = s.srcChannels >= 2 ? 2 : 1;
		s.format = (s.outChannels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

		s.device = alcOpenDevice(nullptr); // default output device
		if (s.device == nullptr)
		{
			wilog_error("stAudio: alcOpenDevice failed - no OpenAL output device.");
			s.src = nullptr;
			return false;
		}
		s.context = alcCreateContext(s.device, nullptr);
		if (s.context == nullptr || alcMakeContextCurrent(s.context) == ALC_FALSE)
		{
			wilog_error("stAudio: failed to create/activate OpenAL context.");
			if (s.context) { alcDestroyContext(s.context); s.context = nullptr; }
			alcCloseDevice(s.device); s.device = nullptr;
			s.src = nullptr;
			return false;
		}

		alGenSources(1, &s.source);
		alGenBuffers(kNumBuffers, s.buffers);
		alSourcef(s.source, AL_GAIN, s.gain.load());

		// Allocate render scratch.
		s.planar.assign(s.srcChannels, std::vector<float>(kFramesPerBuffer, 0.0f));
		s.planarPtrs.resize(s.srcChannels);
		for (int c = 0; c < s.srcChannels; ++c)
			s.planarPtrs[c] = s.planar[c].data();
		s.interleaved.assign((size_t)s.outChannels * kFramesPerBuffer, 0);

		source->Prepare(sampleRate);

		// Prime every buffer, queue them, and start playback before the worker
		// thread takes over the recycle loop.
		for (int i = 0; i < kNumBuffers; ++i)
			s.FillBuffer(s.buffers[i]);
		alSourceQueueBuffers(s.source, kNumBuffers, s.buffers);
		alSourcePlay(s.source);

		s.running.store(true, std::memory_order_release);
		s.worker = std::thread([this] { impl_->Run(); });

		wilog("stAudio: streaming started (%d Hz, %d ch).", sampleRate, s.outChannels);
		return true;
	}

	void DSPStream::Stop()
	{
		Impl& s = *impl_;
		if (!s.running.exchange(false) && s.device == nullptr)
			return; // never started / already stopped

		s.running.store(false);
		if (s.worker.joinable())
			s.worker.join();

		if (s.source)
		{
			alSourceStop(s.source);
			// Detach any queued buffers so they can be deleted.
			alSourcei(s.source, AL_BUFFER, 0);
			alDeleteSources(1, &s.source);
			s.source = 0;
		}
		alDeleteBuffers(kNumBuffers, s.buffers);
		for (int i = 0; i < kNumBuffers; ++i) s.buffers[i] = 0;

		if (s.context)
		{
			alcMakeContextCurrent(nullptr);
			alcDestroyContext(s.context);
			s.context = nullptr;
		}
		if (s.device)
		{
			alcCloseDevice(s.device);
			s.device = nullptr;
		}
		s.src = nullptr;
	}

	bool DSPStream::IsRunning() const { return impl_->running.load(); }

	void DSPStream::SetGain(float gain01)
	{
		if (gain01 < 0.0f) gain01 = 0.0f;
		if (gain01 > 1.0f) gain01 = 1.0f;
		impl_->gain.store(gain01, std::memory_order_relaxed);
	}

	float DSPStream::GetGain() const { return impl_->gain.load(std::memory_order_relaxed); }
}
