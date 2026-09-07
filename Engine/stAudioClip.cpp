#include "stAudioClip.h"
#include "stAudioDecoder.h"
#include "stAudioEngine.h"
#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiVector.h"

#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace st::audio
{
	namespace
	{
		// Frames pulled per decoder call when the length is not known up front.
		constexpr int kDrainChunkFrames = 4096;

		// Run a decoder to completion into one interleaved block. This is the whole
		// difference between a clip and a stream: same decoders, but here the PCM is
		// kept and there it is consumed a block at a time (stAudioStream.h).
		bool DecodeAll(IAudioDecoder& decoder, std::vector<float>& out, int& channels, int& sampleRate)
		{
			channels = decoder.GetChannels();
			sampleRate = decoder.GetSampleRate();
			if (channels <= 0 || sampleRate <= 0)
				return false;

			const int64_t total = decoder.GetTotalFrames();
			if (total > 0)
				out.reserve((size_t)total * (size_t)channels);

			size_t filled = 0;
			for (;;)
			{
				out.resize(filled + (size_t)kDrainChunkFrames * (size_t)channels);
				const int got = decoder.ReadFrames(out.data() + filled, kDrainChunkFrames);
				if (got <= 0)
					break;
				filled += (size_t)got * (size_t)channels;
			}
			out.resize(filled);
			return !out.empty();
		}

		// Linear-interpolated resample, interleaved in and out.
		//
		// Linear is a real quality compromise on large ratio changes - it rolls off the
		// top octave and folds a little alias back in. It is fine here because assets
		// are expected to ship at the mix rate (48 kHz) and this path only catches the
		// odd 22/44.1 kHz file. If a project starts shipping everything at 44.1, the fix
		// is to convert the assets, not to put a polyphase filter on the load path.
		// Opus never reaches it at all: the codec only ever decodes at 48 kHz.
		void Resample(const std::vector<float>& in, int channels, int fromRate, int toRate, std::vector<float>& out)
		{
			if (fromRate == toRate || fromRate <= 0 || toRate <= 0 || channels <= 0)
			{
				out = in;
				return;
			}
			const size_t inFrames = in.size() / (size_t)channels;
			if (inFrames < 2) { out = in; return; }

			const double ratio = (double)toRate / (double)fromRate;
			const size_t outFrames = (size_t)((double)inFrames * ratio);
			out.assign(outFrames * (size_t)channels, 0.0f);

			for (size_t i = 0; i < outFrames; ++i)
			{
				const double srcPos = (double)i / ratio;
				const size_t i0 = (size_t)srcPos;
				const size_t i1 = std::min(i0 + 1, inFrames - 1);
				const float t = (float)(srcPos - (double)i0);
				for (int c = 0; c < channels; ++c)
				{
					const float a = in[i0 * (size_t)channels + (size_t)c];
					const float b = in[i1 * (size_t)channels + (size_t)c];
					out[i * (size_t)channels + (size_t)c] = a + (b - a) * t;
				}
			}
		}

		// cache
		// Weak: the cache never keeps a clip alive on its own, so a level's sounds are
		// freed when the last emitter referencing them goes away, with no eviction
		// policy to tune. TrimClipCache() only sweeps the dead entries.
		std::mutex g_cacheMutex;
		std::unordered_map<std::string, std::weak_ptr<const ClipData>> g_cache;

		std::shared_ptr<ClipData> Build(std::vector<float>&& decoded, int channels, int rate,
			int targetRate, const std::string& name)
		{
			auto clip = std::make_shared<ClipData>();
			clip->name = name;
			clip->channels = channels;
			if (targetRate > 0 && targetRate != rate)
			{
				Resample(decoded, channels, rate, targetRate, clip->samples);
				clip->sampleRate = targetRate;
			}
			else
			{
				clip->samples = std::move(decoded);
				clip->sampleRate = rate;
			}
			return clip;
		}
	}

	AudioClip LoadClipFromMemory(const uint8_t* data, size_t size, const std::string& name, int targetSampleRate)
	{
		if (data == nullptr || size == 0)
		{
			wilog_warning("stAudioClip: empty data for \"%s\".", name.c_str());
			return {};
		}
		if (targetSampleRate <= 0)
			targetSampleRate = GetMixSampleRate();

		// The decoder borrows these bytes rather than copying them; it dies at the end
		// of this function, well inside the caller's ownership of the block.
		AudioDecoderPtr decoder = CreateAudioDecoder(data, size, name);
		if (!decoder)
			return {}; // the decoder factory already logged what was wrong

		std::vector<float> decoded;
		int channels = 0, rate = 0;
		if (!DecodeAll(*decoder, decoded, channels, rate))
		{
			wilog_error("stAudioClip: \"%s\" decoded to nothing.", name.c_str());
			return {};
		}
		return Build(std::move(decoded), channels, rate, targetSampleRate, name);
	}

	AudioClip LoadClip(const std::string& filename, int targetSampleRate)
	{
		if (targetSampleRate <= 0)
			targetSampleRate = GetMixSampleRate();
		const std::string key = filename + "@" + std::to_string(targetSampleRate);
		{
			std::lock_guard<std::mutex> lock(g_cacheMutex);
			auto it = g_cache.find(key);
			if (it != g_cache.end())
			{
				if (AudioClip hit = it->second.lock())
					return hit;
				g_cache.erase(it);
			}
		}

		wi::vector<uint8_t> bytes;
		if (!wi::helper::FileRead(filename, bytes) || bytes.empty())
		{
			wilog_error("stAudioClip: could not read \"%s\".", filename.c_str());
			return {};
		}
		AudioClip clip = LoadClipFromMemory(bytes.data(), bytes.size(), filename, targetSampleRate);
		if (clip)
		{
			std::lock_guard<std::mutex> lock(g_cacheMutex);
			g_cache[key] = clip;
			wilog("stAudioClip: loaded \"%s\" (%.2fs, %d ch, %d Hz).",
				filename.c_str(), clip->GetLengthSeconds(), clip->channels, clip->sampleRate);
		}
		return clip;
	}

	AudioClip CreateClip(const float* interleaved, int frames, int channels, int sampleRate,
		const std::string& name, int targetSampleRate)
	{
		if (interleaved == nullptr || frames <= 0 || channels <= 0 || sampleRate <= 0)
			return {};
		if (targetSampleRate <= 0)
			targetSampleRate = GetMixSampleRate();
		std::vector<float> decoded(interleaved, interleaved + (size_t)frames * (size_t)channels);
		return Build(std::move(decoded), channels, sampleRate, targetSampleRate, name);
	}

	void TrimClipCache()
	{
		std::lock_guard<std::mutex> lock(g_cacheMutex);
		for (auto it = g_cache.begin(); it != g_cache.end();)
			it = it->second.expired() ? g_cache.erase(it) : std::next(it);
	}

	void ClearClipCache()
	{
		std::lock_guard<std::mutex> lock(g_cacheMutex);
		g_cache.clear();
	}
}
