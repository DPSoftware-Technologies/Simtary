#include "stAudioStream.h"
#include "wiBacklog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace st::audio
{
	namespace
	{
		// How much the worker produces per pass. Bigger than one mix block so the
		// thread is not woken for every 10 ms, small enough that a seek lands quickly.
		constexpr int kProduceChunkFrames = 2048;
		// Frames pulled out of the decoder at a time, before downmix.
		constexpr int kDecodeChunkFrames = 1024;
		// Below this much room in the ring it is not worth a pass.
		constexpr int kMinProduceFrames = 64;
	}

	struct StreamPlayer::Impl
	{
		AudioDecoderPtr decoder;
		EmitterRef emitter;
		// Emitter::Input() takes the emitter's clip mutex on every call, so the
		// address is resolved once at open and the worker uses it directly. The
		// emitter outlives the worker - Close() joins before it destroys.
		AudioBuffer* input = nullptr;
		std::thread worker;

		int srcChannels = 0;
		int srcRate = 0;
		int mixRate = 48000;
		int64_t totalFrames = -1;
		AudioFormat format = AudioFormat::Unknown;

		std::atomic<bool> running{ false };
		std::atomic<bool> paused{ false };
		std::atomic<bool> loop{ false };
		std::atomic<bool> finished{ false };
		std::atomic<int64_t> seekRequest{ -1 };
		std::atomic<int64_t> decodedFrames{ 0 };  // decoder cursor, in SOURCE frames

		// Worker-thread only.
		std::vector<float> decodeScratch;  // interleaved, source channels
		std::vector<float> monoQueue;      // downmixed, still at the source rate
		std::vector<float> outScratch;     // resampled, mix rate, mono
		double srcPos = 0.0;               // fractional read position into monoQueue
		bool eof = false;

		~Impl() { StopWorker(); }

		void StopWorker()
		{
			running.store(false, std::memory_order_release);
			if (worker.joinable())
				worker.join();
		}

		bool DecodeMore()
		{
			// Two attempts: the second one only happens after a successful loop-seek,
			// so a decoder that reports end-of-stream from frame zero cannot spin.
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				decodeScratch.resize((size_t)kDecodeChunkFrames * (size_t)srcChannels);
				const int got = decoder->ReadFrames(decodeScratch.data(), kDecodeChunkFrames);
				if (got > 0)
				{
					// Downmix as we go: the emitter's input ring is mono, and summing
					// here is one pass over data already in cache.
					const size_t base = monoQueue.size();
					monoQueue.resize(base + (size_t)got);
					if (srcChannels == 1)
					{
						std::copy(decodeScratch.begin(), decodeScratch.begin() + got, monoQueue.begin() + base);
					}
					else
					{
						const float scale = 1.0f / (float)srcChannels;
						for (int i = 0; i < got; ++i)
						{
							float sum = 0.0f;
							for (int c = 0; c < srcChannels; ++c)
								sum += decodeScratch[(size_t)i * (size_t)srcChannels + (size_t)c];
							monoQueue[base + (size_t)i] = sum * scale;
						}
					}
					decodedFrames.store(decoder->GetFrameOffset(), std::memory_order_relaxed);
					return true;
				}

				if (!loop.load(std::memory_order_relaxed) || !decoder->Seek(0))
					break;
				decodedFrames.store(0, std::memory_order_relaxed);
			}
			eof = true;
			return false;
		}

		void Produce()
		{
			int want = std::min(input->Space(), kProduceChunkFrames);
			if (want < kMinProduceFrames)
				return;

			// Source frames consumed per output frame. Exactly 1.0 for an asset already
			// at the mix rate, in which case the interpolation below is a copy.
			const double ratio = (double)srcRate / (double)mixRate;
			const size_t need = (size_t)std::ceil(srcPos + (double)want * ratio) + 2;
			while (monoQueue.size() < need && !eof)
			{
				if (!DecodeMore())
					break;
			}

			int produced = 0;
			outScratch.resize((size_t)want);
			for (int i = 0; i < want; ++i)
			{
				const size_t i0 = (size_t)srcPos;
				if (i0 + 1 >= monoQueue.size())
					break; // out of decoded input: take what we have and come back
				const float t = (float)(srcPos - (double)i0);
				const float a = monoQueue[i0];
				const float b = monoQueue[i0 + 1];
				outScratch[(size_t)i] = a + (b - a) * t;
				srcPos += ratio;
				++produced;
			}

			if (produced > 0)
				input->WriteMono(outScratch.data(), produced);

			const size_t consumed = (size_t)srcPos;
			if (consumed > 0)
			{
				monoQueue.erase(monoQueue.begin(), monoQueue.begin() + (ptrdiff_t)consumed);
				srcPos -= (double)consumed;
			}

			// Done only once the tail has been handed over AND played out. Checking the
			// ring rather than the decoder is what stops a stream ending a block early.
			if (eof && monoQueue.size() < 2 && input->Available() == 0 && !finished.load(std::memory_order_relaxed))
			{
				finished.store(true, std::memory_order_release);
				// Stop the emitter as well, so a finished stream stops costing a mixer
				// slot and IsPlaying() reads false. Emitter transport is atomics only,
				// which is what makes this safe from the worker thread.
				emitter->Stop();
			}
		}

		void Run()
		{
			while (running.load(std::memory_order_acquire))
			{
				const int64_t seek = seekRequest.exchange(-1, std::memory_order_relaxed);
				if (seek >= 0 && decoder->Seek(seek))
				{
					// The ring is deliberately NOT cleared: its read cursor belongs to
					// the audio thread, and stealing it from here is a data race for the
					// sake of the ~40 ms of already-buffered audio a seek leaves behind.
					monoQueue.clear();
					srcPos = 0.0;
					eof = false;
					finished.store(false, std::memory_order_release);
					decodedFrames.store(seek, std::memory_order_relaxed);
				}

				if (paused.load(std::memory_order_relaxed) || finished.load(std::memory_order_relaxed))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					continue;
				}

				const int spaceBefore = input->Space();
				Produce();
				if (spaceBefore < kMinProduceFrames || eof)
				{
					// Ring full (the mixer has not drained it yet) or nothing left to
					// decode: sleeping a fraction of a block keeps the poll cheap.
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			}
		}
	};

	StreamPlayer::StreamPlayer() : impl_(std::make_unique<Impl>()) {}
	StreamPlayer::~StreamPlayer() { Close(); }

	bool StreamPlayer::Open(const std::string& filename, const Config& config)
	{
		return OpenDecoder(OpenAudioFile(filename), config);
	}

	bool StreamPlayer::OpenFromMemory(std::vector<uint8_t>&& bytes, const Config& config)
	{
		return OpenDecoder(CreateAudioDecoder(std::move(bytes), config.name), config);
	}

	bool StreamPlayer::OpenDecoder(AudioDecoderPtr decoder, const Config& config)
	{
		Close();
		if (!decoder)
			return false;

		AudioEngine& engine = AudioEngine::Get();
		if (!engine.IsInitialized())
		{
			wilog_warning("stAudioStream: \"%s\" - the audio engine is not running.", config.name.c_str());
			return false;
		}

		impl_->srcChannels = decoder->GetChannels();
		impl_->srcRate = decoder->GetSampleRate();
		impl_->totalFrames = decoder->GetTotalFrames();
		impl_->format = decoder->GetFormat();
		impl_->mixRate = engine.GetSampleRate();
		if (impl_->srcChannels <= 0 || impl_->srcRate <= 0 || impl_->mixRate <= 0)
			return false;
		impl_->decoder = std::move(decoder);

		impl_->emitter = engine.CreateEmitter(config.name);
		if (!impl_->emitter)
		{
			impl_->decoder.reset();
			return false;
		}
		impl_->emitter->SetVolume(config.volume);
		impl_->emitter->SetSubmix(config.submix);
		impl_->emitter->SetSpatial(config.spatial);
		// The emitter must never auto-destroy: a pushed-input emitter never reports
		// itself finished, and the player owns its lifetime anyway.
		impl_->emitter->SetAutoDestroy(false);
		// First touch of Input() is what makes this a pushed-samples emitter, so it
		// has to happen before Play() or the mixer would look for a clip.
		impl_->input = &impl_->emitter->Input();

		impl_->loop.store(config.loop, std::memory_order_relaxed);
		impl_->paused.store(false, std::memory_order_relaxed);
		impl_->finished.store(false, std::memory_order_relaxed);
		impl_->seekRequest.store(-1, std::memory_order_relaxed);
		impl_->decodedFrames.store(0, std::memory_order_relaxed);
		impl_->srcPos = 0.0;
		impl_->eof = false;
		impl_->monoQueue.clear();

		impl_->running.store(true, std::memory_order_release);
		impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
		impl_->emitter->Play();

		wilog("stAudioStream: streaming \"%s\" (%s, %d ch, %d Hz, %.1fs).",
			config.name.c_str(), GetAudioFormatName(impl_->format),
			impl_->srcChannels, impl_->srcRate, GetLengthSeconds());
		return true;
	}

	void StreamPlayer::Close()
	{
		impl_->StopWorker();
		if (impl_->emitter)
		{
			impl_->emitter->Stop();
			AudioEngine& engine = AudioEngine::Get();
			if (engine.IsInitialized())
				engine.Destroy(impl_->emitter);
			impl_->emitter.reset();
			impl_->input = nullptr;
		}
		impl_->decoder.reset();
		impl_->monoQueue.clear();
		impl_->monoQueue.shrink_to_fit();
		impl_->finished.store(false, std::memory_order_relaxed);
	}

	bool StreamPlayer::IsOpen() const { return impl_->decoder != nullptr; }
	bool StreamPlayer::IsFinished() const { return impl_->finished.load(std::memory_order_acquire); }

	void StreamPlayer::SetVolume(float volume01)
	{
		if (impl_->emitter)
			impl_->emitter->SetVolume(volume01);
	}

	float StreamPlayer::GetVolume() const
	{
		return impl_->emitter ? impl_->emitter->GetVolume() : 0.0f;
	}

	void StreamPlayer::SetLoop(bool loop) { impl_->loop.store(loop, std::memory_order_relaxed); }
	bool StreamPlayer::GetLoop() const { return impl_->loop.load(std::memory_order_relaxed); }

	void StreamPlayer::SetPaused(bool paused)
	{
		impl_->paused.store(paused, std::memory_order_relaxed);
		if (impl_->emitter)
		{
			if (paused) impl_->emitter->Pause();
			else        impl_->emitter->Play();
		}
	}

	bool StreamPlayer::IsPaused() const { return impl_->paused.load(std::memory_order_relaxed); }

	double StreamPlayer::GetTimeSeconds() const
	{
		if (!impl_->decoder || impl_->srcRate <= 0)
			return 0.0;
		// The decoder leads the speaker by whatever is queued, so subtract it back off.
		const double decoded = (double)impl_->decodedFrames.load(std::memory_order_relaxed)
			/ (double)impl_->srcRate;
		const double buffered = impl_->input
			? (double)impl_->input->Available() / (double)impl_->mixRate : 0.0;
		return std::max(0.0, decoded - buffered);
	}

	double StreamPlayer::GetLengthSeconds() const
	{
		if (impl_->totalFrames < 0 || impl_->srcRate <= 0)
			return -1.0;
		return (double)impl_->totalFrames / (double)impl_->srcRate;
	}

	void StreamPlayer::SeekSeconds(double seconds)
	{
		if (!impl_->decoder || impl_->srcRate <= 0)
			return;
		int64_t frame = (int64_t)(std::max(0.0, seconds) * (double)impl_->srcRate);
		if (impl_->totalFrames >= 0)
			frame = std::min(frame, impl_->totalFrames);
		impl_->seekRequest.store(frame, std::memory_order_relaxed);
	}

	EmitterRef StreamPlayer::GetEmitter() const { return impl_->emitter; }
	AudioFormat StreamPlayer::GetFormat() const { return impl_->format; }

	StreamRef PlayStream(const std::string& filename, const StreamPlayer::Config& config)
	{
		auto player = std::make_shared<StreamPlayer>();
		if (!player->Open(filename, config))
			return {};
		return player;
	}
}
