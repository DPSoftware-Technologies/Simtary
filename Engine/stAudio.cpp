#include "stAudio.h"
#include "stAudioEngine.h"
#include "stAudioComponents.h"
#include "wiBacklog.h"

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>

namespace wi::audio
{
	void InitializeOpenAL()
	{
		// First, and unconditionally: the editor should list the audio components even
		// on a machine with no output device, and this call is also what pulls
		// stAudioComponents.cpp out of the static library at all (see its comment).
		st::RegisterAudioComponents();

		st::audio::EngineConfig config;
		// Reflection and pathing ray tracing is opt-in per emitter, but the simulator
		// has to be built with room for it or a component that asks later gets nothing.
		config.simulation.reflections = true;
		config.simulation.pathing = true;

		if (!st::audio::AudioEngine::Get().Initialize(config))
		{
			wilog_warning("stAudio: audio engine did not start - the game runs silent.");
			return;
		}
	}

	void ShutdownAudio()
	{
		st::audio::AudioEngine::Get().Shutdown();
	}

	void UpdateAudio(float dt)
	{
		st::audio::AudioEngine::Get().Update(dt);
	}

	// DSPStream

	struct DSPStream::Impl
	{
		DSPSource* src = nullptr;
		st::audio::EmitterRef emitter;
		int sampleRate = 48000;
		int srcChannels = 1;
		int frameSize = 512;

		std::thread worker;
		std::atomic<bool> running{ false };
		std::atomic<float> gain{ 1.0f };

		std::vector<std::vector<float>> planar;  // [srcChannels][frameSize]
		std::vector<float*> planarPtrs;
		std::vector<float> mono;

		void Run()
		{
			using clock = std::chrono::steady_clock;
			const double blockSeconds = (double)frameSize / (double)sampleRate;
			auto next = clock::now();

			while (running.load(std::memory_order_acquire))
			{
				st::audio::AudioBuffer& input = emitter->Input();

				// Keep the emitter's ring topped up rather than rendering on a strict
				// clock: the audio thread drains it a block at a time, and this only
				// has to stay ahead of it.
				while (running.load(std::memory_order_relaxed) && input.Space() >= frameSize)
				{
					src->Compute(frameSize, planarPtrs.data());

					const float g = gain.load(std::memory_order_relaxed);
					if (srcChannels == 1)
					{
						const float* s = planar[0].data();
						for (int i = 0; i < frameSize; ++i)
							mono[(size_t)i] = s[i] * g;
					}
					else
					{
						// Downmix: the emitter's input is mono because a spatialized
						// source has one signal, and a 2D emitter is centred anyway.
						const float scale = g / (float)srcChannels;
						for (int i = 0; i < frameSize; ++i)
						{
							float sum = 0.0f;
							for (int c = 0; c < srcChannels; ++c)
								sum += planar[(size_t)c][(size_t)i];
							mono[(size_t)i] = sum * scale;
						}
					}
					input.WriteMono(mono.data(), frameSize);
				}

				next += std::chrono::microseconds((int)(blockSeconds * 500000.0));
				std::this_thread::sleep_until(next);
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

		st::audio::AudioEngine& engine = st::audio::AudioEngine::Get();
		if (!engine.IsInitialized())
		{
			wilog_error("stAudio: DSPStream::Start called before the audio engine came up.");
			return false;
		}

		// The engine's mix rate is authoritative: resampling a live generator would
		// cost more than telling it the right rate in the first place.
		s.sampleRate = engine.GetSampleRate();
		s.frameSize = engine.GetFrameSize();
		if (sampleRate != s.sampleRate)
			wilog("stAudio: DSPStream asked for %d Hz; rendering at the engine's %d Hz instead.",
				sampleRate, s.sampleRate);

		s.src = source;
		s.srcChannels = std::max(1, source->GetNumOutputs());

		s.emitter = engine.CreateEmitter("dsp-stream");
		if (!s.emitter)
			return false;
		// Non-spatial by default: a Faust instrument is a signal, not a place. Callers
		// that want it in the world flip it through GetEmitter().
		s.emitter->SetSpatial(false);
		s.emitter->SetSubmix(st::audio::Submix::SoundEffect);
		s.emitter->Input(); // claim the pushed-samples path before anything reads it

		s.planar.assign((size_t)s.srcChannels, std::vector<float>((size_t)s.frameSize, 0.0f));
		s.planarPtrs.resize((size_t)s.srcChannels);
		for (int c = 0; c < s.srcChannels; ++c)
			s.planarPtrs[(size_t)c] = s.planar[(size_t)c].data();
		s.mono.assign((size_t)s.frameSize, 0.0f);

		source->Prepare(s.sampleRate);

		s.running.store(true, std::memory_order_release);
		s.worker = std::thread([this] { impl_->Run(); });
		s.emitter->Play();

		wilog("stAudio: DSP stream started (%d Hz, %d source channels -> emitter \"%s\").",
			s.sampleRate, s.srcChannels, s.emitter->GetName().c_str());
		return true;
	}

	void DSPStream::Stop()
	{
		Impl& s = *impl_;
		if (!s.running.exchange(false) && !s.emitter)
			return;

		s.running.store(false);
		if (s.worker.joinable())
			s.worker.join();

		if (s.emitter)
		{
			st::audio::AudioEngine::Get().Destroy(s.emitter);
			s.emitter.reset();
		}
		s.src = nullptr;
	}

	bool DSPStream::IsRunning() const { return impl_->running.load(); }

	void DSPStream::SetGain(float gain01)
	{
		impl_->gain.store(std::clamp(gain01, 0.0f, 1.0f), std::memory_order_relaxed);
	}

	float DSPStream::GetGain() const { return impl_->gain.load(std::memory_order_relaxed); }

	std::shared_ptr<st::audio::Emitter> DSPStream::GetEmitter() const { return impl_->emitter; }
}
