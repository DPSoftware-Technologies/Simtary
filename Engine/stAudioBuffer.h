#pragma once
// stAudioBuffer: the sample interchange between the game and the audio thread
//
// One class, two jobs, picked with Mode:
//
//	Mode::Stream  the game (or a DSP generator) PRODUCES samples and the audio thread
//	              CONSUMES them. A full ring drops the newest write rather than
//	              blocking - the audio thread must never wait on a game thread.
//	              This is what an Emitter's input buffer is: push mono samples in,
//	              the mixer pulls them out block by block.
//
//	Mode::Tap     the audio thread PRODUCES and the game CONSUMES, and the producer
//	              never stalls: a full ring overwrites its oldest frames. Readers ask
//	              for "the most recent N frames" and get a snapshot without consuming,
//	              so two readers (a VU meter and a recorder) do not steal from each
//	              other. This is what a Collector's output buffer is.
//
// Layout is PLANAR float - one contiguous array per channel - because that is what
// both Faust (`dsp::compute`) and Steam Audio (`IPLAudioBuffer`) want. Interleaving
// happens once, at the very end, when a block is handed to OpenAL.
//
// Thread safety: single-producer / single-consumer. The index pair is atomic and the
// data race is resolved by the ring discipline alone, so neither side takes a lock.
// More than one producer, or more than one consumer on a Stream buffer, is undefined.

#include <atomic>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace st::audio
{
	class AudioBuffer
	{
	public:
		enum class Mode
		{
			Stream, // producer = game, consumer = audio thread; overflow drops the write
			Tap,    // producer = audio thread, consumer = game; overflow overwrites the oldest
		};

		AudioBuffer() = default;
		AudioBuffer(int channels, int capacityFrames, Mode mode) { Reset(channels, capacityFrames, mode); }

		// (Re)allocate. Not thread safe - call it while nothing is reading or writing
		// (component Start(), or under the engine's mixer lock).
		void Reset(int channels, int capacityFrames, Mode mode)
		{
			channels_ = channels < 1 ? 1 : channels;
			// One spare frame keeps "full" and "empty" distinguishable without a count.
			capacity_ = capacityFrames < 2 ? 2 : capacityFrames + 1;
			mode_ = mode;
			planes_.assign((size_t)channels_, std::vector<float>((size_t)capacity_, 0.0f));
			planePtrs_.resize((size_t)channels_);
			for (int c = 0; c < channels_; ++c)
				planePtrs_[(size_t)c] = planes_[(size_t)c].data();
			write_.store(0, std::memory_order_relaxed);
			read_.store(0, std::memory_order_relaxed);
			written_.store(0, std::memory_order_relaxed);
		}

		int  GetChannels() const { return channels_; }
		int  GetCapacityFrames() const { return capacity_ - 1; }
		Mode GetMode() const { return mode_; }
		bool IsValid() const { return channels_ > 0 && capacity_ > 1; }

		// Frames available to Read(). For a Tap buffer this saturates at capacity.
		int Available() const
		{
			const int w = write_.load(std::memory_order_acquire);
			const int r = read_.load(std::memory_order_relaxed);
			return w >= r ? (w - r) : (w - r + capacity_);
		}

		// Frames Write() can take before it starts dropping (Stream) or overwriting (Tap).
		int Space() const { return capacity_ - 1 - Available(); }

		// Total frames ever written. Cheap "is anything happening" probe for a meter.
		uint64_t GetTotalFramesWritten() const { return written_.load(std::memory_order_relaxed); }

		void Clear()
		{
			read_.store(write_.load(std::memory_order_relaxed), std::memory_order_release);
		}

		// producer side

		// Write planar frames. `src` holds GetChannels() pointers (a null plane writes
		// silence for that channel, which is how a mono source feeds a stereo buffer).
		// Returns frames actually written: always `frames` for a Tap buffer, possibly
		// fewer for a Stream buffer that ran out of room.
		int Write(const float* const* src, int frames)
		{
			if (!IsValid() || frames <= 0) return 0;
			if (mode_ == Mode::Stream)
			{
				frames = std::min(frames, Space());
				if (frames <= 0) return 0;
			}
			else if (frames > capacity_ - 1)
			{
				// A single write larger than the whole ring: keep only its tail.
				const int skip = frames - (capacity_ - 1);
				const float* shifted[kMaxPlanes];
				const int n = std::min(channels_, (int)kMaxPlanes);
				for (int c = 0; c < n; ++c)
					shifted[c] = src[c] ? src[c] + skip : nullptr;
				return Write(shifted, capacity_ - 1);
			}

			int w = write_.load(std::memory_order_relaxed);
			const int first = std::min(frames, capacity_ - w);
			const int second = frames - first;
			for (int c = 0; c < channels_; ++c)
			{
				float* dst = planes_[(size_t)c].data();
				const float* s = src[c];
				if (s)
				{
					std::memcpy(dst + w, s, (size_t)first * sizeof(float));
					if (second > 0) std::memcpy(dst, s + first, (size_t)second * sizeof(float));
				}
				else
				{
					std::memset(dst + w, 0, (size_t)first * sizeof(float));
					if (second > 0) std::memset(dst, 0, (size_t)second * sizeof(float));
				}
			}
			w = (w + frames) % capacity_;
			write_.store(w, std::memory_order_release);
			written_.fetch_add((uint64_t)frames, std::memory_order_relaxed);

			// Tap: the producer owns the read cursor too when it laps it.
			if (mode_ == Mode::Tap)
			{
				const int r = read_.load(std::memory_order_relaxed);
				const int used = w >= r ? (w - r) : (w - r + capacity_);
				if (used > capacity_ - 1)
					read_.store((w + 1) % capacity_, std::memory_order_release);
			}
			return frames;
		}

		// Convenience: push an interleaved block (what a decoder or a network packet
		// hands you). `srcChannels` may differ from the buffer's - extra channels are
		// dropped, a mono source is copied to every channel.
		int WriteInterleaved(const float* src, int frames, int srcChannels)
		{
			if (!IsValid() || src == nullptr || frames <= 0 || srcChannels < 1) return 0;
			scratch_.resize((size_t)channels_ * (size_t)frames);
			const float* planes[kMaxPlanes];
			const int n = std::min(channels_, (int)kMaxPlanes);
			for (int c = 0; c < n; ++c)
			{
				float* p = scratch_.data() + (size_t)c * (size_t)frames;
				const int sc = (srcChannels == 1) ? 0 : std::min(c, srcChannels - 1);
				for (int i = 0; i < frames; ++i)
					p[i] = src[(size_t)i * (size_t)srcChannels + (size_t)sc];
				planes[c] = p;
			}
			return Write(planes, frames);
		}

		// Mono helper - the common case for a 3D emitter.
		int WriteMono(const float* src, int frames)
		{
			const float* planes[kMaxPlanes];
			const int n = std::min(channels_, (int)kMaxPlanes);
			for (int c = 0; c < n; ++c) planes[c] = src;
			return Write(planes, frames);
		}

		// consumer side

		// Consume up to `frames` planar frames into `dst` (GetChannels() pointers; a
		// null plane is skipped). Frames not available are zero-filled so the caller
		// always gets a full block. Returns how many were real.
		int Read(float* const* dst, int frames)
		{
			if (!IsValid() || frames <= 0) return 0;
			const int have = std::min(frames, Available());
			int r = read_.load(std::memory_order_relaxed);
			const int first = std::min(have, capacity_ - r);
			const int second = have - first;
			for (int c = 0; c < channels_; ++c)
			{
				float* d = dst[c];
				if (d == nullptr) continue;
				const float* s = planes_[(size_t)c].data();
				std::memcpy(d, s + r, (size_t)first * sizeof(float));
				if (second > 0) std::memcpy(d + first, s, (size_t)second * sizeof(float));
				if (have < frames) std::memset(d + have, 0, (size_t)(frames - have) * sizeof(float));
			}
			read_.store((r + have) % capacity_, std::memory_order_release);
			return have;
		}

		// Snapshot the most recent `frames` WITHOUT consuming - Unity's
		// AudioSource.GetOutputData. Older-than-available frames come back as silence.
		// Safe to call from the game thread against a Tap buffer the audio thread is
		// still filling: worst case the newest frame or two is torn, which no meter,
		// waveform or FFT will notice.
		int Peek(float* const* dst, int frames) const
		{
			if (!IsValid() || frames <= 0) return 0;
			const int have = std::min(frames, Available());
			const int w = write_.load(std::memory_order_acquire);
			int start = w - have;
			if (start < 0) start += capacity_;
			const int first = std::min(have, capacity_ - start);
			const int second = have - first;
			for (int c = 0; c < channels_; ++c)
			{
				float* d = dst[c];
				if (d == nullptr) continue;
				const float* s = planes_[(size_t)c].data();
				const int pad = frames - have;
				if (pad > 0) std::memset(d, 0, (size_t)pad * sizeof(float));
				std::memcpy(d + pad, s + start, (size_t)first * sizeof(float));
				if (second > 0) std::memcpy(d + pad + first, s, (size_t)second * sizeof(float));
			}
			return have;
		}

		// Single-channel Peek into a flat array - the one-liner behind
		// GetOutputData(channel) and a level meter.
		int PeekChannel(int channel, float* dst, int frames) const
		{
			if (channel < 0 || channel >= channels_ || dst == nullptr) return 0;
			float* planes[kMaxPlanes] = {};
			planes[channel] = dst;
			return Peek(planes, frames);
		}

		// Peak absolute sample over the most recent `frames`, all channels. What a VU
		// meter or a "is this emitter actually making noise" check wants.
		float GetPeak(int frames = 512) const
		{
			if (!IsValid()) return 0.0f;
			const int have = std::min(frames, Available());
			if (have <= 0) return 0.0f;
			const int w = write_.load(std::memory_order_acquire);
			int start = w - have;
			if (start < 0) start += capacity_;
			float peak = 0.0f;
			for (int c = 0; c < channels_; ++c)
			{
				const float* s = planes_[(size_t)c].data();
				for (int i = 0; i < have; ++i)
				{
					const float v = std::abs(s[(start + i) % capacity_]);
					if (v > peak) peak = v;
				}
			}
			return peak;
		}

		// Root-mean-square over the most recent `frames`, averaged across channels.
		float GetRMS(int frames = 512) const
		{
			if (!IsValid()) return 0.0f;
			const int have = std::min(frames, Available());
			if (have <= 0) return 0.0f;
			const int w = write_.load(std::memory_order_acquire);
			int start = w - have;
			if (start < 0) start += capacity_;
			double sum = 0.0;
			for (int c = 0; c < channels_; ++c)
			{
				const float* s = planes_[(size_t)c].data();
				for (int i = 0; i < have; ++i)
				{
					const double v = s[(start + i) % capacity_];
					sum += v * v;
				}
			}
			return (float)std::sqrt(sum / (double)(have * channels_));
		}

	private:
		// Steam Audio's own ceiling is ambisonics order 3 = 16 channels; the stack
		// plane arrays are sized for that so nothing here ever heap-allocates per block.
		static constexpr size_t kMaxPlanes = 16;

		int channels_ = 0;
		int capacity_ = 0;
		Mode mode_ = Mode::Stream;
		std::vector<std::vector<float>> planes_;
		std::vector<float*> planePtrs_;
		std::vector<float> scratch_;   // deinterleave staging, producer-thread only
		std::atomic<int> write_{ 0 };
		std::atomic<int> read_{ 0 };
		std::atomic<uint64_t> written_{ 0 };
	};
}
