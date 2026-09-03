#include "stAudioEngine.h"
#include "wiBacklog.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace st::audio
{
	namespace
	{
		// AL_EXT_FLOAT32 tokens. alext.h defines them, but the extension is optional
		// and some SDK headers ship without it, so they are pinned here rather than
		// making the build depend on which header landed.
		constexpr ALenum kFormatMonoFloat32 = 0x10010;
		constexpr ALenum kFormatStereoFloat32 = 0x10011;

		inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
		inline float ClampSample(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

		// Drain and log the AL error queue. Called at the seams, not per sample.
		void CheckAL(const char* where)
		{
			for (ALenum err = alGetError(); err != AL_NO_ERROR; err = alGetError())
				wilog_warning("stAudioEngine: OpenAL error 0x%04X at %s.", (unsigned)err, where);
		}
	}

	// ════════════════════════════════════════════════════════════════════════════
	// Emitter
	// ════════════════════════════════════════════════════════════════════════════

	struct Emitter::Impl
	{
		std::string name;

		// Source. A clip and a pushed input are exclusive; `usesInput` records which
		// one won so the audio thread does not have to guess.
		mutable std::mutex clipMutex;
		AudioClip clip;
		bool usesInput = false;
		AudioBuffer input;    // Stream: game writes, audio thread reads
		AudioBuffer output;   // Tap: audio thread writes, game reads

		// Transport. `cursor` is fractional because pitch resamples on the fly.
		std::atomic<bool> playing{ false };
		std::atomic<bool> paused{ false };
		std::atomic<bool> finished{ false };
		double cursor = 0.0;
		std::atomic<double> seekTo{ -1.0 };
		float delayRemaining = 0.0f;

		// Play / loop regions, in SECONDS of clip time. A negative length means "to the
		// end of the clip"; they are resolved to frames per pull, because the clip can
		// be swapped under a playing emitter.
		std::atomic<float> regionBegin{ 0.0f };
		std::atomic<float> regionLength{ -1.0f };
		std::atomic<float> loopRegionBegin{ -1.0f };  // negative = fall back to regionBegin
		std::atomic<float> loopRegionLength{ -1.0f };

		std::atomic<float> volume{ 1.0f };
		std::atomic<float> pitch{ 1.0f };
		std::atomic<bool> loop{ false };
		std::atomic<uint32_t> submix{ (uint32_t)Submix::SoundEffect };
		std::atomic<bool> spatial{ true };
		std::atomic<bool> autoDestroy{ false };

		// 3D
		mutable std::mutex spatialMutex;
		EmitterSpatialSettings spatialSettings;
		SpatialTransform transform;
		SpatialSource source;
		std::atomic<bool> spatialDirty{ true };

		// 2D routing: a non-spatial emitter gets its own OpenAL streaming voice, so
		// OpenAL does the 2D mixing exactly as it does for the one-shot pool.
		ALuint alSource = 0;
		std::vector<ALuint> alBuffers;
		bool alPrimed = false;

		// Audio-thread scratch, allocated once at creation.
		std::vector<float> block;

		// Read one mono block from whichever source is live. Returns false when a
		// non-looping clip has run out, which is what triggers auto-destroy.
		bool PullMono(float* dst, int frames, int sampleRate)
		{
			std::memset(dst, 0, (size_t)frames * sizeof(float));

			if (usesInput)
			{
				float* planes[1] = { dst };
				input.Read(planes, frames);
				return true;
			}

			AudioClip local;
			{
				std::lock_guard<std::mutex> lock(clipMutex);
				local = clip;
			}
			if (!local || local->channels <= 0)
				return true;

			const double want = seekTo.exchange(-1.0, std::memory_order_relaxed);
			if (want >= 0.0)
				cursor = want * (double)local->sampleRate;

			const int channels = local->channels;
			const int totalFrames = local->GetFrameCount();
			if (totalFrames <= 0)
				return false;

			// Resolve the play and loop regions into clip frames.
			const double clipRate = (double)local->sampleRate;
			double firstFrame = (double)regionBegin.load(std::memory_order_relaxed) * clipRate;
			const float regionLen = regionLength.load(std::memory_order_relaxed);
			double lastFrame = (regionLen > 0.0f) ? firstFrame + (double)regionLen * clipRate : (double)totalFrames;
			firstFrame = std::clamp(firstFrame, 0.0, (double)totalFrames);
			lastFrame = std::clamp(lastFrame, firstFrame + 1.0, (double)totalFrames);

			const float loopBeginSec = loopRegionBegin.load(std::memory_order_relaxed);
			const float loopLenSec = loopRegionLength.load(std::memory_order_relaxed);
			double loopStart = (loopBeginSec >= 0.0f) ? (double)loopBeginSec * clipRate : firstFrame;
			double loopEnd = (loopLenSec > 0.0f) ? loopStart + (double)loopLenSec * clipRate : lastFrame;
			loopStart = std::clamp(loopStart, firstFrame, lastFrame - 1.0);
			loopEnd = std::clamp(loopEnd, loopStart + 1.0, lastFrame);

			if (cursor < firstFrame)
				cursor = firstFrame;

			// Pitch also rescales the clip's own rate against the mix rate, so a 44.1 kHz
			// clip that slipped past the loader still plays at the right speed.
			double step = (double)pitch.load(std::memory_order_relaxed);
			if (step <= 0.0) step = 0.0001;
			step *= (double)local->sampleRate / (double)sampleRate;

			const float* samples = local->samples.data();
			const bool looping = loop.load(std::memory_order_relaxed);

			// The end that stops playback is the play region's; the end that WRAPS is the
			// loop region's, and they are not the same edge - a sound can play an intro
			// from firstFrame and then loop a shorter tail forever.
			const double wrapEnd = looping ? loopEnd : lastFrame;
			for (int i = 0; i < frames; ++i)
			{
				if (cursor >= wrapEnd)
				{
					if (!looping)
						return false;
					const double span = loopEnd - loopStart;
					cursor = loopStart + std::fmod(cursor - loopStart, span);
				}
				const size_t i0 = (size_t)cursor;
				const size_t i1 = (cursor + 1.0 < wrapEnd) ? i0 + 1 : (looping ? (size_t)loopStart : i0);
				const float t = (float)(cursor - (double)i0);

				// Downmix to mono: a point in space radiates one signal, so a stereo
				// asset on a 3D emitter loses its image here rather than being panned
				// twice. Load stereo assets on 2D emitters if the image matters.
				float a = 0.0f, b = 0.0f;
				for (int c = 0; c < channels; ++c)
				{
					a += samples[i0 * (size_t)channels + (size_t)c];
					b += samples[i1 * (size_t)channels + (size_t)c];
				}
				a /= (float)channels;
				b /= (float)channels;
				dst[i] = a + (b - a) * t;
				cursor += step;
			}
			return true;
		}
	};

	Emitter::Emitter() : impl_(std::make_unique<Impl>()) {}
	Emitter::~Emitter() = default;

	void Emitter::SetClip(const AudioClip& clip)
	{
		std::lock_guard<std::mutex> lock(impl_->clipMutex);
		impl_->clip = clip;
		impl_->usesInput = false;
		impl_->cursor = 0.0;
	}

	AudioClip Emitter::GetClip() const
	{
		std::lock_guard<std::mutex> lock(impl_->clipMutex);
		return impl_->clip;
	}

	AudioBuffer& Emitter::Input()
	{
		// First touch of Input() is what declares this a pushed-samples emitter.
		std::lock_guard<std::mutex> lock(impl_->clipMutex);
		impl_->usesInput = true;
		impl_->clip.reset();
		return impl_->input;
	}

	const AudioBuffer& Emitter::Output() const { return impl_->output; }
	AudioBuffer& Emitter::Output() { return impl_->output; }

	void Emitter::Play()
	{
		impl_->finished.store(false, std::memory_order_relaxed);
		impl_->paused.store(false, std::memory_order_relaxed);
		impl_->playing.store(true, std::memory_order_release);
	}
	void Emitter::PlayDelayed(float seconds) { impl_->delayRemaining = seconds; Play(); }
	void Emitter::Pause() { impl_->paused.store(true, std::memory_order_release); }
	void Emitter::Stop()
	{
		impl_->playing.store(false, std::memory_order_release);
		impl_->paused.store(false, std::memory_order_relaxed);
		// Rewind to the region begin rather than to zero: an emitter with a play region
		// that restarted at 0 would play audio its region deliberately excludes.
		impl_->seekTo.store((double)impl_->regionBegin.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
	}
	bool Emitter::IsPlaying() const
	{
		return impl_->playing.load(std::memory_order_acquire) && !impl_->paused.load(std::memory_order_relaxed);
	}
	bool Emitter::IsPaused() const { return impl_->paused.load(std::memory_order_relaxed); }

	float Emitter::GetTime() const
	{
		AudioClip local = GetClip();
		if (!local || local->sampleRate <= 0) return 0.0f;
		return (float)(impl_->cursor / (double)local->sampleRate);
	}
	void Emitter::SetTime(float seconds) { impl_->seekTo.store((double)seconds, std::memory_order_relaxed); }

	void Emitter::SetPlayRegion(float beginSeconds, float lengthSeconds)
	{
		impl_->regionBegin.store(std::max(0.0f, beginSeconds), std::memory_order_relaxed);
		impl_->regionLength.store(lengthSeconds > 0.0f ? lengthSeconds : -1.0f, std::memory_order_relaxed);
	}

	void Emitter::SetLoopRegion(float beginSeconds, float lengthSeconds)
	{
		impl_->loopRegionBegin.store(beginSeconds >= 0.0f ? beginSeconds : -1.0f, std::memory_order_relaxed);
		impl_->loopRegionLength.store(lengthSeconds > 0.0f ? lengthSeconds : -1.0f, std::memory_order_relaxed);
	}

	void Emitter::SetVolume(float v) { impl_->volume.store(Clamp01(v), std::memory_order_relaxed); }
	float Emitter::GetVolume() const { return impl_->volume.load(std::memory_order_relaxed); }
	void Emitter::SetPitch(float p) { impl_->pitch.store(p, std::memory_order_relaxed); }
	float Emitter::GetPitch() const { return impl_->pitch.load(std::memory_order_relaxed); }
	void Emitter::SetLoop(bool l) { impl_->loop.store(l, std::memory_order_relaxed); }
	bool Emitter::GetLoop() const { return impl_->loop.load(std::memory_order_relaxed); }
	void Emitter::SetSubmix(Submix s) { impl_->submix.store((uint32_t)s, std::memory_order_relaxed); }
	Submix Emitter::GetSubmix() const { return (Submix)impl_->submix.load(std::memory_order_relaxed); }
	void Emitter::SetSpatial(bool s) { impl_->spatial.store(s, std::memory_order_release); }
	bool Emitter::IsSpatial() const { return impl_->spatial.load(std::memory_order_acquire); }
	void Emitter::SetAutoDestroy(bool v) { impl_->autoDestroy.store(v, std::memory_order_relaxed); }

	EmitterSpatialSettings& Emitter::SpatialSettings() { return impl_->spatialSettings; }
	const EmitterSpatialSettings& Emitter::SpatialSettings() const { return impl_->spatialSettings; }
	void Emitter::ApplySpatialSettings() { impl_->spatialDirty.store(true, std::memory_order_release); }

	void Emitter::SetTransform(const SpatialTransform& transform)
	{
		{
			std::lock_guard<std::mutex> lock(impl_->spatialMutex);
			impl_->transform = transform;
		}
		if (impl_->spatialDirty.exchange(false, std::memory_order_acq_rel))
			impl_->source.SetSettings(impl_->spatialSettings);
		impl_->source.SetTransform(transform);
	}

	void Emitter::SetPosition(const XMFLOAT3& position)
	{
		SpatialTransform t;
		{
			std::lock_guard<std::mutex> lock(impl_->spatialMutex);
			t = impl_->transform;
		}
		t.position = position;
		SetTransform(t);
	}

	const SpatialTransform& Emitter::GetTransform() const { return impl_->transform; }
	SpatialResult Emitter::GetSpatialResult() const { return impl_->source.GetResult(); }
	void Emitter::SetName(const std::string& name) { impl_->name = name; }
	const std::string& Emitter::GetName() const { return impl_->name; }

	// ════════════════════════════════════════════════════════════════════════════
	// Collector
	// ════════════════════════════════════════════════════════════════════════════

	struct Collector::Impl
	{
		std::string name;
		SpatialRenderer renderer;
		CollectorSpatialSettings settings;
		SpatialTransform transform;
		mutable std::mutex mutex;

		AudioBuffer output;   // Tap: this microphone's own render
		std::atomic<float> volume{ 1.0f };
		std::atomic<int>  priority{ 0 };
		std::atomic<bool> primary{ false };
		std::atomic<bool> routeToOutput{ true };
		std::atomic<bool> enabled{ true };
		std::atomic<bool> settingsDirty{ true };

		// Audio-thread scratch: one plane per output channel.
		std::vector<std::vector<float>> planes;
		std::vector<float*> planePtrs;
	};

	Collector::Collector() : impl_(std::make_unique<Impl>()) {}
	Collector::~Collector() = default;

	const AudioBuffer& Collector::Output() const { return impl_->output; }
	AudioBuffer& Collector::Output() { return impl_->output; }
	CollectorSpatialSettings& Collector::SpatialSettings() { return impl_->settings; }
	const CollectorSpatialSettings& Collector::SpatialSettings() const { return impl_->settings; }
	void Collector::ApplySpatialSettings() { impl_->settingsDirty.store(true, std::memory_order_release); }

	void Collector::SetTransform(const SpatialTransform& transform)
	{
		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->transform = transform;
		}
		// Only the primary drives the ray tracer's point of view; a secondary
		// microphone that published here would make the simulator jump between rooms.
		if (impl_->primary.load(std::memory_order_relaxed))
			impl_->renderer.SetTransform(transform);
	}

	const SpatialTransform& Collector::GetTransform() const { return impl_->transform; }
	void Collector::SetVolume(float v) { impl_->volume.store(Clamp01(v), std::memory_order_relaxed); }
	float Collector::GetVolume() const { return impl_->volume.load(std::memory_order_relaxed); }
	void Collector::SetPriority(int p) { impl_->priority.store(p, std::memory_order_relaxed); }
	int Collector::GetPriority() const { return impl_->priority.load(std::memory_order_relaxed); }
	bool Collector::IsPrimary() const { return impl_->primary.load(std::memory_order_relaxed); }
	void Collector::SetRouteToOutput(bool v) { impl_->routeToOutput.store(v, std::memory_order_relaxed); }
	bool Collector::GetRouteToOutput() const { return impl_->routeToOutput.load(std::memory_order_relaxed); }
	void Collector::SetEnabled(bool v) { impl_->enabled.store(v, std::memory_order_release); }
	bool Collector::IsEnabled() const { return impl_->enabled.load(std::memory_order_acquire); }
	void Collector::SetName(const std::string& name) { impl_->name = name; }
	const std::string& Collector::GetName() const { return impl_->name; }

	// ════════════════════════════════════════════════════════════════════════════
	// AudioEngine
	// ════════════════════════════════════════════════════════════════════════════

	struct AudioEngine::Impl
	{
		EngineConfig config;
		bool initialized = false;
		std::string deviceName;

		ALCdevice* device = nullptr;
		ALCcontext* context = nullptr;
		bool floatSupported = false;

		// The one streaming source that carries the whole Steam Audio render.
		ALuint spatialSource = 0;
		std::vector<ALuint> spatialBuffers;

		// 2D one-shot pool: OpenAL static voices, relative to the listener so OpenAL
		// mixes them without spatializing them.
		struct Voice2D
		{
			ALuint source = 0;
			uint32_t submix = (uint32_t)Submix::UI;
			float volume = 1.0f;
			bool busy = false;
		};
		std::vector<Voice2D> voices2D;
		std::unordered_map<const ClipData*, ALuint> clipBufferCache;
		std::mutex voiceMutex;

		// Registries. The game thread mutates these; the audio thread rebuilds its own
		// private snapshot when `listsDirty` is set, so a block never waits on a
		// registration.
		mutable std::mutex registryMutex;
		std::vector<EmitterRef> emitters;
		std::vector<CollectorRef> collectors;
		std::atomic<bool> listsDirty{ true };
		std::vector<EmitterRef> activeEmitters;      // audio thread only
		std::vector<CollectorRef> activeCollectors;  // audio thread only

		std::atomic<float> masterVolume{ 1.0f };
		std::atomic<float> submixVolume[(size_t)Submix::Count];
		std::atomic<bool> paused{ false };

		std::thread audioThread;
		std::thread simThread;
		std::atomic<bool> running{ false };

		// Mix scratch, sized once at Initialize.
		std::vector<float> monoScratch;
		std::vector<float> mixL, mixR;
		std::vector<float> interleaved;
		std::vector<int16_t> interleaved16;

		mutable std::mutex statsMutex;
		Stats stats;

		EmitterRef music;

		// ── audio thread ────────────────────────────────────────────────────────

		void RefreshLists()
		{
			if (!listsDirty.exchange(false, std::memory_order_acq_rel))
				return;
			std::lock_guard<std::mutex> lock(registryMutex);
			activeEmitters = emitters;
			activeCollectors = collectors;
		}

		void RenderBlock()
		{
			const int frames = config.frameSize;
			const int rate = config.sampleRate;
			const float master = masterVolume.load(std::memory_order_relaxed);

			RefreshLists();

			std::fill(mixL.begin(), mixL.end(), 0.0f);
			std::fill(mixR.begin(), mixR.end(), 0.0f);

			// Only collectors that are enabled AND have a working renderer take part.
			for (auto& collector : activeCollectors)
			{
				Collector::Impl& c = *collector->impl_;
				if (!c.enabled.load(std::memory_order_acquire)) continue;
				if (c.settingsDirty.exchange(false, std::memory_order_acq_rel))
					c.renderer.SetSettings(c.settings);
				if (!c.renderer.IsValid()) continue;
				c.renderer.BeginBlock(frames);
			}

			int activeCount = 0, spatialCount = 0;
			for (auto& emitter : activeEmitters)
			{
				Emitter::Impl& e = *emitter->impl_;
				if (!e.playing.load(std::memory_order_acquire) || e.paused.load(std::memory_order_relaxed))
					continue;

				float* mono = monoScratch.data();
				const bool alive = e.PullMono(mono, frames, rate);
				if (!alive)
				{
					e.playing.store(false, std::memory_order_release);
					e.finished.store(true, std::memory_order_release);
					continue;
				}

				const uint32_t submix = e.submix.load(std::memory_order_relaxed);
				const float gain = e.volume.load(std::memory_order_relaxed)
					* submixVolume[std::min<size_t>(submix, (size_t)Submix::Count - 1)].load(std::memory_order_relaxed)
					* master;
				for (int i = 0; i < frames; ++i)
					mono[i] *= gain;

				// The emitter's own tap: what it emitted, before anything spatial.
				e.output.WriteMono(mono, frames);
				++activeCount;

				if (e.spatial.load(std::memory_order_acquire))
				{
					++spatialCount;
					// Cull against the emitter's own maxDistance per collector: a
					// microphone across the map should not pay for this source.
					for (auto& collector : activeCollectors)
					{
						Collector::Impl& c = *collector->impl_;
						if (!c.enabled.load(std::memory_order_acquire) || !c.renderer.IsValid()) continue;
						c.renderer.Accumulate(e.source, mono, frames, 1.0f);
					}
				}
				else
				{
					// Non-spatial emitter with no OpenAL voice of its own (the pool was
					// exhausted, or the driver refused a source): fold it into the
					// stereo bus rather than dropping the sound.
					if (e.alSource == 0)
					{
						for (int i = 0; i < frames; ++i)
						{
							mixL[(size_t)i] += mono[i];
							mixR[(size_t)i] += mono[i];
						}
					}
					else
					{
						StreamTo2DVoice(e, mono, frames);
					}
				}
			}

			int collectorCount = 0;
			float peak = 0.0f;
			for (auto& collector : activeCollectors)
			{
				Collector::Impl& c = *collector->impl_;
				if (!c.enabled.load(std::memory_order_acquire) || !c.renderer.IsValid()) continue;
				++collectorCount;

				const int channels = c.renderer.GetOutputChannels();
				if ((int)c.planes.size() != channels)
				{
					c.planes.assign((size_t)channels, std::vector<float>((size_t)frames, 0.0f));
					c.planePtrs.resize((size_t)channels);
					for (int ch = 0; ch < channels; ++ch)
						c.planePtrs[(size_t)ch] = c.planes[(size_t)ch].data();
				}
				c.renderer.EndBlock(c.planePtrs.data(), frames);

				const float volume = c.volume.load(std::memory_order_relaxed);
				if (volume != 1.0f)
					for (int ch = 0; ch < channels; ++ch)
						for (int i = 0; i < frames; ++i)
							c.planes[(size_t)ch][(size_t)i] *= volume;

				// Every collector fills its own tap, primary or not. That is the whole
				// point of a Collector that is not the player's ears.
				c.output.Write(c.planePtrs.data(), frames);

				if (c.primary.load(std::memory_order_relaxed) && c.routeToOutput.load(std::memory_order_relaxed))
				{
					const float* L = c.planes[0].data();
					const float* R = c.planes[(size_t)std::min(1, channels - 1)].data();
					for (int i = 0; i < frames; ++i)
					{
						mixL[(size_t)i] += L[i];
						mixR[(size_t)i] += R[i];
					}
				}
			}

			for (int i = 0; i < frames; ++i)
			{
				peak = std::max(peak, std::max(std::abs(mixL[(size_t)i]), std::abs(mixR[(size_t)i])));
			}

			{
				std::lock_guard<std::mutex> lock(statsMutex);
				stats.activeEmitters = activeCount;
				stats.spatialEmitters = spatialCount;
				stats.activeCollectors = collectorCount;
				stats.peakOutput = peak;
				++stats.blocksRendered;
			}
		}

		// Queue one block onto a non-spatial emitter's own OpenAL streaming voice.
		void StreamTo2DVoice(Emitter::Impl& e, const float* mono, int frames)
		{
			ALint processed = 0;
			alGetSourcei(e.alSource, AL_BUFFERS_PROCESSED, &processed);
			ALuint buffer = 0;
			if (processed > 0)
			{
				alSourceUnqueueBuffers(e.alSource, 1, &buffer);
			}
			else if (!e.alPrimed)
			{
				// Priming pass: the queue is still filling for the first time.
				ALint queued = 0;
				alGetSourcei(e.alSource, AL_BUFFERS_QUEUED, &queued);
				if (queued < (ALint)e.alBuffers.size())
					buffer = e.alBuffers[(size_t)queued];
				else
					e.alPrimed = true;
			}
			if (buffer == 0)
				return; // the voice is saturated; this block is simply late, not lost

			if (floatSupported)
			{
				alBufferData(buffer, kFormatMonoFloat32, mono, (ALsizei)(frames * sizeof(float)), config.sampleRate);
			}
			else
			{
				for (int i = 0; i < frames; ++i)
					interleaved16[(size_t)i] = (int16_t)(ClampSample(mono[i]) * 32767.0f);
				alBufferData(buffer, AL_FORMAT_MONO16, interleaved16.data(),
					(ALsizei)(frames * sizeof(int16_t)), config.sampleRate);
			}
			alSourceQueueBuffers(e.alSource, 1, &buffer);

			ALint state = 0;
			alGetSourcei(e.alSource, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING)
				alSourcePlay(e.alSource);
		}

		void SubmitToDevice()
		{
			const int frames = config.frameSize;
			ALint processed = 0;
			alGetSourcei(spatialSource, AL_BUFFERS_PROCESSED, &processed);
			if (processed <= 0)
				return;

			ALuint buffer = 0;
			alSourceUnqueueBuffers(spatialSource, 1, &buffer);

			if (floatSupported)
			{
				for (int i = 0; i < frames; ++i)
				{
					interleaved[(size_t)(2 * i + 0)] = ClampSample(mixL[(size_t)i]);
					interleaved[(size_t)(2 * i + 1)] = ClampSample(mixR[(size_t)i]);
				}
				alBufferData(buffer, kFormatStereoFloat32, interleaved.data(),
					(ALsizei)(interleaved.size() * sizeof(float)), config.sampleRate);
			}
			else
			{
				for (int i = 0; i < frames; ++i)
				{
					interleaved16[(size_t)(2 * i + 0)] = (int16_t)(ClampSample(mixL[(size_t)i]) * 32767.0f);
					interleaved16[(size_t)(2 * i + 1)] = (int16_t)(ClampSample(mixR[(size_t)i]) * 32767.0f);
				}
				alBufferData(buffer, AL_FORMAT_STEREO16, interleaved16.data(),
					(ALsizei)(2 * frames * sizeof(int16_t)), config.sampleRate);
			}
			alSourceQueueBuffers(spatialSource, 1, &buffer);

			ALint state = 0;
			alGetSourcei(spatialSource, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING)
			{
				ALint queued = 0;
				alGetSourcei(spatialSource, AL_BUFFERS_QUEUED, &queued);
				if (queued > 0)
				{
					alSourcePlay(spatialSource);
					std::lock_guard<std::mutex> lock(statsMutex);
					++stats.underruns;
				}
			}
		}

		void AudioThread()
		{
			// The context is process-wide and was made current on the thread that ran
			// Initialize; it stays valid here.
			const double blockSeconds = (double)config.frameSize / (double)config.sampleRate;
			while (running.load(std::memory_order_acquire))
			{
				if (paused.load(std::memory_order_relaxed))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}

				ALint processed = 0;
				alGetSourcei(spatialSource, AL_BUFFERS_PROCESSED, &processed);
				if (processed <= 0)
				{
					// Nothing to refill yet. Sleeping a fraction of a block keeps the
					// poll cheap without risking an underrun.
					std::this_thread::sleep_for(std::chrono::microseconds((int)(blockSeconds * 250000.0)));
					continue;
				}

				const auto begin = std::chrono::steady_clock::now();
				while (processed-- > 0)
				{
					RenderBlock();
					SubmitToDevice();
				}
				const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
				{
					std::lock_guard<std::mutex> lock(statsMutex);
					stats.mixLoad = (float)(elapsed / blockSeconds);
				}
			}
		}

		void SimThread()
		{
			const float hz = std::max(1.0f, config.simulation.updateRateHz);
			const auto period = std::chrono::milliseconds((int)(1000.0f / hz));
			while (running.load(std::memory_order_acquire))
			{
				if (!paused.load(std::memory_order_relaxed))
					Spatializer::Get().RunSimulation();
				std::this_thread::sleep_for(period);
			}
		}

		// ── 2D voice pool ───────────────────────────────────────────────────────

		// Upload a clip into an AL buffer once and keep it. Cached by ClipData
		// address; the cache holds no reference, so it is swept when clips are freed.
		ALuint GetOrCreateALBuffer(const AudioClip& clip)
		{
			if (!clip || clip->samples.empty())
				return 0;
			auto it = clipBufferCache.find(clip.get());
			if (it != clipBufferCache.end())
				return it->second;

			ALuint buffer = 0;
			alGenBuffers(1, &buffer);
			if (buffer == 0)
				return 0;

			const int channels = std::min(clip->channels, 2);
			const int frames = clip->GetFrameCount();
			if (floatSupported)
			{
				std::vector<float> data((size_t)frames * (size_t)channels);
				for (int i = 0; i < frames; ++i)
					for (int c = 0; c < channels; ++c)
						data[(size_t)i * (size_t)channels + (size_t)c] =
							clip->samples[(size_t)i * (size_t)clip->channels + (size_t)c];
				alBufferData(buffer, channels == 1 ? kFormatMonoFloat32 : kFormatStereoFloat32,
					data.data(), (ALsizei)(data.size() * sizeof(float)), clip->sampleRate);
			}
			else
			{
				std::vector<int16_t> data((size_t)frames * (size_t)channels);
				for (int i = 0; i < frames; ++i)
					for (int c = 0; c < channels; ++c)
						data[(size_t)i * (size_t)channels + (size_t)c] = (int16_t)(ClampSample(
							clip->samples[(size_t)i * (size_t)clip->channels + (size_t)c]) * 32767.0f);
				alBufferData(buffer, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
					data.data(), (ALsizei)(data.size() * sizeof(int16_t)), clip->sampleRate);
			}
			clipBufferCache[clip.get()] = buffer;
			return buffer;
		}

		void PlayOneShot2D(const AudioClip& clip, float volume, Submix submix, float pitch)
		{
			if (!initialized || !clip)
				return;
			std::lock_guard<std::mutex> lock(voiceMutex);
			const ALuint buffer = GetOrCreateALBuffer(clip);
			if (buffer == 0)
				return;

			for (auto& voice : voices2D)
			{
				ALint state = 0;
				alGetSourcei(voice.source, AL_SOURCE_STATE, &state);
				if (state == AL_PLAYING || state == AL_PAUSED)
					continue;

				voice.submix = (uint32_t)submix;
				voice.volume = Clamp01(volume);
				alSourcei(voice.source, AL_BUFFER, (ALint)buffer);
				alSourcef(voice.source, AL_PITCH, pitch);
				alSourcef(voice.source, AL_GAIN, voice.volume
					* submixVolume[(size_t)submix].load(std::memory_order_relaxed)
					* masterVolume.load(std::memory_order_relaxed));
				alSourcePlay(voice.source);
				voice.busy = true;
				return;
			}
			wilog_warning("stAudioEngine: 2D voice pool exhausted (%d voices) - one-shot dropped.",
				(int)voices2D.size());
		}

		// Give a non-spatial emitter its own OpenAL streaming voice, and take it back
		// when the emitter turns spatial. OpenAL owns 2D mixing in this engine, so a
		// 2D emitter belongs to it rather than to the Steam Audio bus. Runs on the main
		// thread from Update(): the audio thread only ever queues onto a voice that
		// already exists, and reads `alSource` after this has published it.
		void SyncEmitterVoice(Emitter::Impl& e)
		{
			const bool wantsVoice = !e.spatial.load(std::memory_order_acquire);
			if (wantsVoice && e.alSource == 0)
			{
				ALuint source = 0;
				alGenSources(1, &source);
				if (source == 0)
				{
					// Out of sources: RenderBlock folds the emitter into the stereo bus
					// instead. Quieter failure than dropping the sound.
					alGetError();
					return;
				}
				alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
				alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
				alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
				e.alBuffers.resize((size_t)std::max(2, config.queuedBlocks));
				alGenBuffers((ALsizei)e.alBuffers.size(), e.alBuffers.data());
				e.alPrimed = false;
				e.alSource = source;
			}
			else if (!wantsVoice && e.alSource != 0)
			{
				alSourceStop(e.alSource);
				alSourcei(e.alSource, AL_BUFFER, 0);
				alDeleteSources(1, &e.alSource);
				if (!e.alBuffers.empty())
					alDeleteBuffers((ALsizei)e.alBuffers.size(), e.alBuffers.data());
				e.alBuffers.clear();
				e.alSource = 0;
				e.alPrimed = false;
			}

			if (e.alSource != 0)
			{
				// Unity gain on the voice: RenderBlock has already folded emitter,
				// submix and master volume into the samples, and applying them twice
				// is the classic way to end up with a mix that squares its own fader.
				alSourcef(e.alSource, AL_GAIN, 1.0f);
				if (!e.playing.load(std::memory_order_acquire) || e.paused.load(std::memory_order_relaxed))
					alSourcePause(e.alSource);
			}
		}

		// OpenAL handles belonging to an emitter that has just been destroyed. The
		// audio thread may still be inside a block that was queuing onto them, so they
		// are released a few Updates later instead of immediately.
		struct RetiredVoice
		{
			ALuint source = 0;
			std::vector<ALuint> buffers;
			int framesLeft = 3;
		};
		std::vector<RetiredVoice> retiredVoices;

		void RetireVoice(Emitter::Impl& e)
		{
			if (e.alSource == 0) return;
			RetiredVoice retired;
			retired.source = e.alSource;
			retired.buffers = e.alBuffers;
			retiredVoices.push_back(std::move(retired));
			e.alSource = 0;
			e.alBuffers.clear();
			e.alPrimed = false;
		}

		void FlushRetiredVoices()
		{
			for (auto it = retiredVoices.begin(); it != retiredVoices.end();)
			{
				if (--it->framesLeft > 0) { ++it; continue; }
				alSourceStop(it->source);
				alSourcei(it->source, AL_BUFFER, 0);
				alDeleteSources(1, &it->source);
				if (!it->buffers.empty())
					alDeleteBuffers((ALsizei)it->buffers.size(), it->buffers.data());
				it = retiredVoices.erase(it);
			}
		}

		// A 2D voice's gain has to track the submix and master sliders while it plays,
		// so the pool is refreshed once per frame rather than only at trigger time.
		void RefreshVoiceGains()
		{
			std::lock_guard<std::mutex> lock(voiceMutex);
			const float master = masterVolume.load(std::memory_order_relaxed);
			int inUse = 0;
			for (auto& voice : voices2D)
			{
				ALint state = 0;
				alGetSourcei(voice.source, AL_SOURCE_STATE, &state);
				if (state != AL_PLAYING)
				{
					voice.busy = false;
					continue;
				}
				++inUse;
				alSourcef(voice.source, AL_GAIN, voice.volume
					* submixVolume[std::min<size_t>(voice.submix, (size_t)Submix::Count - 1)].load(std::memory_order_relaxed)
					* master);
			}
			std::lock_guard<std::mutex> statsLock(statsMutex);
			stats.voices2D = inUse;
		}
	};

	AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>())
	{
		for (size_t i = 0; i < (size_t)Submix::Count; ++i)
			impl_->submixVolume[i].store(1.0f);
	}
	AudioEngine::~AudioEngine() { Shutdown(); }

	AudioEngine& AudioEngine::Get()
	{
		static AudioEngine instance;
		return instance;
	}

	bool AudioEngine::IsInitialized() const { return impl_->initialized; }
	const EngineConfig& AudioEngine::GetConfig() const { return impl_->config; }
	int AudioEngine::GetSampleRate() const { return impl_->config.sampleRate; }
	int AudioEngine::GetFrameSize() const { return impl_->config.frameSize; }
	const char* AudioEngine::GetDeviceName() const { return impl_->deviceName.c_str(); }
	bool AudioEngine::IsSpatialAvailable() const { return Spatializer::Get().IsSteamAudioAvailable(); }

	bool AudioEngine::Initialize(const EngineConfig& config)
	{
		Impl& s = *impl_;
		if (s.initialized)
			return true;
		s.config = config;

		s.device = alcOpenDevice(nullptr);
		if (s.device == nullptr)
		{
			wilog_warning("stAudioEngine: no OpenAL output device - audio disabled.");
			return false;
		}

		// Ask the driver to run at the mix rate so nothing resamples behind our back.
		const ALCint attributes[] = { ALC_FREQUENCY, config.sampleRate, 0 };
		s.context = alcCreateContext(s.device, attributes);
		if (s.context == nullptr || alcMakeContextCurrent(s.context) == ALC_FALSE)
		{
			wilog_error("stAudioEngine: failed to create/activate the OpenAL context.");
			if (s.context) { alcDestroyContext(s.context); s.context = nullptr; }
			alcCloseDevice(s.device); s.device = nullptr;
			return false;
		}

		const ALCchar* name = nullptr;
		if (alcIsExtensionPresent(s.device, "ALC_ENUMERATE_ALL_EXT") == ALC_TRUE)
			name = alcGetString(s.device, ALC_ALL_DEVICES_SPECIFIER);
		if (name == nullptr)
			name = alcGetString(s.device, ALC_DEFAULT_DEVICE_SPECIFIER);
		s.deviceName = name ? name : "default";

		s.floatSupported = config.floatOutput && alIsExtensionPresent("AL_EXT_FLOAT32") == AL_TRUE;

		// The spatial bus. Everything about this source says "do not spatialize me":
		// relative to the listener, parked at the origin, no rolloff, no distance
		// model. Steam Audio has already done all of that.
		alGenSources(1, &s.spatialSource);
		alSourcei(s.spatialSource, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f(s.spatialSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
		alSource3f(s.spatialSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
		alSourcef(s.spatialSource, AL_ROLLOFF_FACTOR, 0.0f);
		alSourcef(s.spatialSource, AL_GAIN, 1.0f);
		alDistanceModel(AL_NONE);

		s.spatialBuffers.resize((size_t)std::max(2, config.queuedBlocks));
		alGenBuffers((ALsizei)s.spatialBuffers.size(), s.spatialBuffers.data());

		// 2D pool. Same "do not spatialize" setup - OpenAL is the mixer here, not a
		// panner.
		s.voices2D.resize((size_t)std::max(1, config.maxVoices2D));
		for (auto& voice : s.voices2D)
		{
			alGenSources(1, &voice.source);
			if (voice.source == 0)
				continue;
			alSourcei(voice.source, AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(voice.source, AL_POSITION, 0.0f, 0.0f, 0.0f);
			alSourcef(voice.source, AL_ROLLOFF_FACTOR, 0.0f);
		}
		s.voices2D.erase(std::remove_if(s.voices2D.begin(), s.voices2D.end(),
			[](const Impl::Voice2D& v) { return v.source == 0; }), s.voices2D.end());
		CheckAL("Initialize");

		// Scratch.
		s.monoScratch.assign((size_t)config.frameSize, 0.0f);
		s.mixL.assign((size_t)config.frameSize, 0.0f);
		s.mixR.assign((size_t)config.frameSize, 0.0f);
		s.interleaved.assign((size_t)config.frameSize * 2, 0.0f);
		s.interleaved16.assign((size_t)config.frameSize * 2, 0);

		Spatializer::Get().Initialize(config.sampleRate, config.frameSize, config.simulation);

		// Prime the queue with silence so the audio thread starts in the steady state
		// (it only ever refills buffers OpenAL has handed back).
		for (ALuint buffer : s.spatialBuffers)
		{
			if (s.floatSupported)
				alBufferData(buffer, kFormatStereoFloat32, s.interleaved.data(),
					(ALsizei)(s.interleaved.size() * sizeof(float)), config.sampleRate);
			else
				alBufferData(buffer, AL_FORMAT_STEREO16, s.interleaved16.data(),
					(ALsizei)(s.interleaved16.size() * sizeof(int16_t)), config.sampleRate);
		}
		alSourceQueueBuffers(s.spatialSource, (ALsizei)s.spatialBuffers.size(), s.spatialBuffers.data());
		alSourcePlay(s.spatialSource);

		s.initialized = true;
		s.running.store(true, std::memory_order_release);
		s.audioThread = std::thread([this] { impl_->AudioThread(); });
		s.simThread = std::thread([this] { impl_->SimThread(); });

		wilog("stAudioEngine: up on \"%s\" (%d Hz, %d-frame blocks x %d, %s output, %d 2D voices, 3D via %s).",
			s.deviceName.c_str(), config.sampleRate, config.frameSize, (int)s.spatialBuffers.size(),
			s.floatSupported ? "float32" : "int16", (int)s.voices2D.size(),
			Spatializer::Get().GetBackendName());
		return true;
	}

	void AudioEngine::Shutdown()
	{
		Impl& s = *impl_;
		if (!s.initialized)
			return;

		s.running.store(false, std::memory_order_release);
		if (s.audioThread.joinable()) s.audioThread.join();
		if (s.simThread.joinable()) s.simThread.join();
		s.initialized = false;

		{
			std::lock_guard<std::mutex> lock(s.registryMutex);
			s.emitters.clear();
			s.collectors.clear();
		}
		s.activeEmitters.clear();
		s.activeCollectors.clear();
		s.music.reset();

		Spatializer::Get().Shutdown();

		if (s.spatialSource)
		{
			alSourceStop(s.spatialSource);
			alSourcei(s.spatialSource, AL_BUFFER, 0);
			alDeleteSources(1, &s.spatialSource);
			s.spatialSource = 0;
		}
		if (!s.spatialBuffers.empty())
		{
			alDeleteBuffers((ALsizei)s.spatialBuffers.size(), s.spatialBuffers.data());
			s.spatialBuffers.clear();
		}
		for (auto& voice : s.voices2D)
		{
			alSourceStop(voice.source);
			alSourcei(voice.source, AL_BUFFER, 0);
			alDeleteSources(1, &voice.source);
		}
		s.voices2D.clear();
		for (auto& entry : s.clipBufferCache)
			alDeleteBuffers(1, &entry.second);
		s.clipBufferCache.clear();

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
		wilog("stAudioEngine: shut down.");
	}

	void AudioEngine::Update(float dt)
	{
		Impl& s = *impl_;
		if (!s.initialized)
			return;

		// Elect the primary collector: highest priority among the enabled ones. This
		// is also what decides whose point of view the ray tracer simulates from.
		CollectorRef primary;
		int bestPriority = 0;
		std::vector<EmitterRef> finished;
		{
			std::lock_guard<std::mutex> lock(s.registryMutex);
			for (auto& collector : s.collectors)
			{
				if (!collector->IsEnabled()) continue;
				const int priority = collector->GetPriority();
				if (!primary || priority > bestPriority)
				{
					primary = collector;
					bestPriority = priority;
				}
			}
			for (auto& collector : s.collectors)
				collector->impl_->primary.store(collector == primary, std::memory_order_relaxed);

			for (auto& emitter : s.emitters)
			{
				if (emitter->impl_->autoDestroy.load(std::memory_order_relaxed) &&
					emitter->impl_->finished.load(std::memory_order_acquire))
					finished.push_back(emitter);
				s.SyncEmitterVoice(*emitter->impl_);
			}
		}
		if (primary)
			primary->impl_->renderer.SetTransform(primary->GetTransform());

		for (auto& emitter : finished)
			Destroy(emitter);

		// Geometry the game added this frame becomes visible to the ray tracer here,
		// once, rather than per AddStaticMesh call.
		Spatializer::Get().CommitScene();
		s.RefreshVoiceGains();
		s.FlushRetiredVoices();
		(void)dt;
	}

	EmitterRef AudioEngine::CreateEmitter(const std::string& name)
	{
		Impl& s = *impl_;
		EmitterRef emitter(new Emitter());
		emitter->SetName(name);

		const int frames = s.config.frameSize > 0 ? s.config.frameSize : 512;
		// Four blocks of input slack lets a game thread running at 60 Hz feed a 512-
		// frame audio thread without ever starving it; the tap is longer so a recorder
		// polling once a frame never misses a block.
		emitter->impl_->input.Reset(1, frames * 4, AudioBuffer::Mode::Stream);
		emitter->impl_->output.Reset(1, frames * 8, AudioBuffer::Mode::Tap);
		emitter->impl_->block.assign((size_t)frames, 0.0f);
		emitter->impl_->source.Create(emitter->impl_->spatialSettings);

		std::lock_guard<std::mutex> lock(s.registryMutex);
		s.emitters.push_back(emitter);
		s.listsDirty.store(true, std::memory_order_release);
		return emitter;
	}

	CollectorRef AudioEngine::CreateCollector(const std::string& name)
	{
		Impl& s = *impl_;
		CollectorRef collector(new Collector());
		collector->SetName(name);

		const int frames = s.config.frameSize > 0 ? s.config.frameSize : 512;
		collector->impl_->renderer.Create(collector->impl_->settings);
		const int channels = collector->impl_->renderer.GetOutputChannels();
		collector->impl_->output.Reset(channels, frames * 8, AudioBuffer::Mode::Tap);

		std::lock_guard<std::mutex> lock(s.registryMutex);
		s.collectors.push_back(collector);
		s.listsDirty.store(true, std::memory_order_release);
		return collector;
	}

	void AudioEngine::Destroy(const EmitterRef& emitter)
	{
		if (!emitter) return;
		Impl& s = *impl_;
		emitter->Stop();
		std::lock_guard<std::mutex> lock(s.registryMutex);
		s.RetireVoice(*emitter->impl_);
		s.emitters.erase(std::remove(s.emitters.begin(), s.emitters.end(), emitter), s.emitters.end());
		s.listsDirty.store(true, std::memory_order_release);
		// The emitter's Steam Audio source is released by its destructor, which runs
		// only once the audio thread has dropped its snapshot - so a block already in
		// flight cannot be left holding a freed IPLSource.
	}

	void AudioEngine::Destroy(const CollectorRef& collector)
	{
		if (!collector) return;
		Impl& s = *impl_;
		collector->SetEnabled(false);
		std::lock_guard<std::mutex> lock(s.registryMutex);
		s.collectors.erase(std::remove(s.collectors.begin(), s.collectors.end(), collector), s.collectors.end());
		s.listsDirty.store(true, std::memory_order_release);
	}

	CollectorRef AudioEngine::GetPrimaryCollector() const
	{
		std::lock_guard<std::mutex> lock(impl_->registryMutex);
		for (auto& collector : impl_->collectors)
			if (collector->IsPrimary())
				return collector;
		return {};
	}

	void AudioEngine::GetEmitters(std::vector<EmitterRef>& out) const
	{
		std::lock_guard<std::mutex> lock(impl_->registryMutex);
		out = impl_->emitters;
	}

	void AudioEngine::GetCollectors(std::vector<CollectorRef>& out) const
	{
		std::lock_guard<std::mutex> lock(impl_->registryMutex);
		out = impl_->collectors;
	}

	void AudioEngine::SetMasterVolume(float v) { impl_->masterVolume.store(Clamp01(v), std::memory_order_relaxed); }
	float AudioEngine::GetMasterVolume() const { return impl_->masterVolume.load(std::memory_order_relaxed); }

	void AudioEngine::SetSubmixVolume(Submix submix, float v)
	{
		if ((size_t)submix >= (size_t)Submix::Count) return;
		impl_->submixVolume[(size_t)submix].store(Clamp01(v), std::memory_order_relaxed);
	}

	float AudioEngine::GetSubmixVolume(Submix submix) const
	{
		if ((size_t)submix >= (size_t)Submix::Count) return 0.0f;
		return impl_->submixVolume[(size_t)submix].load(std::memory_order_relaxed);
	}

	void AudioEngine::SetPaused(bool paused)
	{
		impl_->paused.store(paused, std::memory_order_release);
		if (!impl_->initialized) return;
		if (paused) alSourcePause(impl_->spatialSource);
		else        alSourcePlay(impl_->spatialSource);
	}
	bool AudioEngine::IsPaused() const { return impl_->paused.load(std::memory_order_relaxed); }

	AudioEngine::Stats AudioEngine::GetStats() const
	{
		std::lock_guard<std::mutex> lock(impl_->statsMutex);
		return impl_->stats;
	}

	// ── the easy API ────────────────────────────────────────────────────────────

	int GetMixSampleRate()
	{
		AudioEngine& engine = AudioEngine::Get();
		return engine.IsInitialized() ? engine.GetSampleRate() : EngineConfig{}.sampleRate;
	}

	int GetMixFrameSize()
	{
		AudioEngine& engine = AudioEngine::Get();
		return engine.IsInitialized() ? engine.GetFrameSize() : EngineConfig{}.frameSize;
	}

	void PlayOneShot(const AudioClip& clip, float volume, Submix submix, float pitch)
	{
		AudioEngine::Get().impl_->PlayOneShot2D(clip, volume, submix, pitch);
	}

	void PlayOneShot(const std::string& filename, float volume, Submix submix, float pitch)
	{
		PlayOneShot(LoadClip(filename), volume, submix, pitch);
	}

	EmitterRef PlayClipAtPoint(const AudioClip& clip, const XMFLOAT3& position, float volume, Submix submix)
	{
		if (!clip)
			return {};
		EmitterRef emitter = AudioEngine::Get().CreateEmitter("one-shot");
		emitter->SetClip(clip);
		emitter->SetVolume(volume);
		emitter->SetSubmix(submix);
		emitter->SetSpatial(true);
		emitter->SetAutoDestroy(true);
		emitter->SetPosition(position);
		emitter->Play();
		return emitter;
	}

	EmitterRef PlayClipAtPoint(const std::string& filename, const XMFLOAT3& position, float volume, Submix submix)
	{
		return PlayClipAtPoint(LoadClip(filename), position, volume, submix);
	}

	EmitterRef PlayMusic(const std::string& filename, float volume, bool loop)
	{
		AudioEngine& engine = AudioEngine::Get();
		StopMusic();
		if (filename.empty())
			return {};
		AudioClip clip = LoadClip(filename);
		if (!clip)
			return {};
		EmitterRef emitter = engine.CreateEmitter("music");
		emitter->SetClip(clip);
		emitter->SetVolume(volume);
		emitter->SetLoop(loop);
		emitter->SetSubmix(Submix::Music);
		emitter->SetSpatial(false);   // music is not in the world
		emitter->Play();
		engine.impl_->music = emitter;
		return emitter;
	}

	void StopMusic()
	{
		AudioEngine& engine = AudioEngine::Get();
		if (engine.impl_->music)
		{
			engine.Destroy(engine.impl_->music);
			engine.impl_->music.reset();
		}
	}

	void StopAll()
	{
		AudioEngine& engine = AudioEngine::Get();
		std::vector<EmitterRef> emitters;
		engine.GetEmitters(emitters);
		for (auto& emitter : emitters)
			emitter->Stop();
		engine.impl_->music.reset();

		std::lock_guard<std::mutex> lock(engine.impl_->voiceMutex);
		for (auto& voice : engine.impl_->voices2D)
			alSourceStop(voice.source);
	}
}
