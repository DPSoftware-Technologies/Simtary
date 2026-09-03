#include "stAudioClip.h"
#include "stAudioEngine.h"
#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiVector.h"

// stb_vorbis is compiled into FAudio and declared header-only here, exactly the way
// wiAudio.cpp consumes it - including the .c a second time would duplicate every
// symbol at link time.
#define STB_VORBIS_HEADER_ONLY
#include "Utility/stb_vorbis.c"

#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace st::audio
{
	namespace
	{
		// ── little-endian readers ────────────────────────────────────────────────
		// RIFF is little-endian on every platform, so the bytes are assembled by hand
		// rather than memcpy'd into a native integer.
		inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
		inline uint32_t rd32(const uint8_t* p)
		{
			return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		}
		inline bool tag(const uint8_t* p, const char* fourcc)
		{
			return p[0] == (uint8_t)fourcc[0] && p[1] == (uint8_t)fourcc[1]
				&& p[2] == (uint8_t)fourcc[2] && p[3] == (uint8_t)fourcc[3];
		}

		// WAVE_FORMAT_* tags we accept.
		constexpr uint16_t kFormatPCM = 0x0001;
		constexpr uint16_t kFormatFloat = 0x0003;
		constexpr uint16_t kFormatExtensible = 0xFFFE;

		// Decode RIFF/WAVE into interleaved float. Returns false (quietly - the caller
		// then tries Ogg) when the data is not a RIFF container at all.
		bool DecodeWav(const uint8_t* data, size_t size, std::vector<float>& out, int& channels, int& sampleRate)
		{
			if (size < 12 || !tag(data, "RIFF") || !tag(data + 8, "WAVE"))
				return false;

			uint16_t format = 0, bits = 0, numChannels = 0;
			uint32_t rate = 0;
			const uint8_t* pcm = nullptr;
			size_t pcmBytes = 0;

			// Walk the chunk list. Chunks are word-aligned, so an odd size is followed
			// by a pad byte that is not counted in the size field.
			size_t pos = 12;
			while (pos + 8 <= size)
			{
				const uint8_t* id = data + pos;
				const uint32_t chunkSize = rd32(data + pos + 4);
				const uint8_t* body = data + pos + 8;
				if (pos + 8 + (size_t)chunkSize > size)
					break; // truncated file: keep whatever complete chunks we already have

				if (tag(id, "fmt "))
				{
					if (chunkSize < 16) return false;
					format = rd16(body);
					numChannels = rd16(body + 2);
					rate = rd32(body + 4);
					bits = rd16(body + 14);
					if (format == kFormatExtensible && chunkSize >= 40)
					{
						// The real format lives in the first two bytes of the GUID.
						format = rd16(body + 24);
					}
				}
				else if (tag(id, "data"))
				{
					pcm = body;
					pcmBytes = (size_t)chunkSize;
				}
				pos += 8 + (size_t)chunkSize + ((chunkSize & 1u) ? 1u : 0u);
			}

			if (pcm == nullptr || numChannels == 0 || rate == 0)
				return false;
			if (format != kFormatPCM && format != kFormatFloat)
			{
				wilog_warning("stAudioClip: unsupported WAV format tag 0x%04X (compressed WAV is not decoded).", format);
				return false;
			}

			const int bytesPerSample = bits / 8;
			if (bytesPerSample <= 0) return false;
			const size_t total = pcmBytes / (size_t)bytesPerSample;
			out.resize(total);

			switch (bits)
			{
			case 8: // WAV 8-bit is UNSIGNED, unlike every other width
				for (size_t i = 0; i < total; ++i)
					out[i] = ((float)pcm[i] - 128.0f) / 128.0f;
				break;
			case 16:
				for (size_t i = 0; i < total; ++i)
					out[i] = (float)(int16_t)rd16(pcm + i * 2) / 32768.0f;
				break;
			case 24:
				for (size_t i = 0; i < total; ++i)
				{
					const uint8_t* s = pcm + i * 3;
					int32_t v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16));
					if (v & 0x800000) v |= (int32_t)0xFF000000; // sign-extend 24 -> 32
					out[i] = (float)v / 8388608.0f;
				}
				break;
			case 32:
				if (format == kFormatFloat)
				{
					for (size_t i = 0; i < total; ++i)
					{
						const uint32_t bitsLE = rd32(pcm + i * 4);
						float f; std::memcpy(&f, &bitsLE, sizeof(f));
						out[i] = f;
					}
				}
				else
				{
					for (size_t i = 0; i < total; ++i)
						out[i] = (float)(int32_t)rd32(pcm + i * 4) / 2147483648.0f;
				}
				break;
			default:
				wilog_warning("stAudioClip: unsupported WAV bit depth %u.", (unsigned)bits);
				return false;
			}

			channels = (int)numChannels;
			sampleRate = (int)rate;
			return true;
		}

		bool DecodeOgg(const uint8_t* data, size_t size, std::vector<float>& out, int& channels, int& sampleRate)
		{
			if (size < 4 || !tag(data, "OggS"))
				return false;
			short* pcm = nullptr;
			const int frames = stb_vorbis_decode_memory(data, (int)size, &channels, &sampleRate, &pcm);
			if (frames <= 0 || pcm == nullptr)
				return false;
			const size_t total = (size_t)frames * (size_t)channels;
			out.resize(total);
			for (size_t i = 0; i < total; ++i)
				out[i] = (float)pcm[i] / 32768.0f;
			std::free(pcm); // stb_vorbis allocates with malloc
			return true;
		}

		// Linear-interpolated resample, interleaved in and out.
		//
		// Linear is a real quality compromise on large ratio changes - it rolls off the
		// top octave and folds a little alias back in. It is fine here because assets
		// are expected to ship at the mix rate (48 kHz) and this path only catches the
		// odd 22/44.1 kHz file. If a project starts shipping everything at 44.1, the fix
		// is to convert the assets, not to put a polyphase filter on the load path.
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

		// ── cache ────────────────────────────────────────────────────────────────
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

		std::vector<float> decoded;
		int channels = 0, rate = 0;
		if (!DecodeWav(data, size, decoded, channels, rate) &&
			!DecodeOgg(data, size, decoded, channels, rate))
		{
			wilog_error("stAudioClip: \"%s\" is neither RIFF/WAVE nor Ogg Vorbis (or is corrupt).", name.c_str());
			return {};
		}
		if (channels <= 0 || rate <= 0 || decoded.empty())
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
