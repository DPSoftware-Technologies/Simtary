#include "stAudioDecoder.h"
#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiVector.h"

// stb_vorbis is compiled into FAudio (Utility/FAudio/src/XNA_Song.c) and declared
// header-only here, exactly the way stAudioClip.cpp used to consume it - including
// the .c a second time would duplicate every symbol at link time. Only the
// float/pulldata entry points are used, which is what FAudio's build leaves in:
// its translation unit sets STB_VORBIS_NO_INTEGER_CONVERSION.
#define STB_VORBIS_HEADER_ONLY
#include "Utility/stb_vorbis.c"

// QOA's reference implementation. stdlib.h is pulled in first so its own include
// guard is already closed by the time the extern "C" block opens - a C++ standard
// header included INSIDE extern "C" is what breaks builds, and qoa.h's
// implementation section includes stdlib.h unguarded.
//
// qoa.h wraps only its DECLARATIONS in extern "C"; the implementation below the
// guard is left with whatever linkage the including file has. Wrapping the include
// gives both halves C linkage and keeps them agreeing.
#include <cstdlib>

// The reference implementation is C, and assigns malloc's void* straight to a typed
// pointer - which C++ rejects. QOA_MALLOC / QOA_FREE are documented hooks, so the fix
// is to supply ones that convert rather than to fork the header: qoa.h stays
// byte-identical to upstream and a future drop-in needs no re-patching.
//
// The proxy has to live OUTSIDE the extern "C" block below, because a template cannot
// be declared with C language linkage.
namespace st::audio::qoa_detail
{
	struct MallocProxy
	{
		void* p;
		template <typename T> operator T* () const { return static_cast<T*>(p); }
	};
	inline MallocProxy Malloc(size_t bytes) { return MallocProxy{ std::malloc(bytes) }; }
}
#define QOA_MALLOC(sz) ::st::audio::qoa_detail::Malloc((size_t)(sz))
#define QOA_FREE(p)    std::free(p)

extern "C" {
#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "Utility/qoa.h"
}

#if defined(SIMTARY_HAS_OPUS)
#include <opus.h>
#include <opus_multistream.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>

namespace st::audio
{
	namespace
	{
		// little-endian readers
		// Every container here is little-endian except Ogg's checksum, which is not
		// verified, so the bytes are assembled by hand rather than memcpy'd into a
		// native integer.
		inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
		inline uint32_t rd32(const uint8_t* p)
		{
			return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		}
		inline uint64_t rd64(const uint8_t* p)
		{
			return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
		}
		inline bool tag(const uint8_t* p, const char* fourcc)
		{
			return p[0] == (uint8_t)fourcc[0] && p[1] == (uint8_t)fourcc[1]
				&& p[2] == (uint8_t)fourcc[2] && p[3] == (uint8_t)fourcc[3];
		}
		inline bool tag8(const uint8_t* p, const char* eight)
		{
			return std::memcmp(p, eight, 8) == 0;
		}

		inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

		// Base for every decoder: holds the compressed bytes, and owns them when the
		// caller handed them over. `data_`/`size_` is what the decoders actually read,
		// so a borrowed block and an owned one take the same path.
		class DecoderBase : public IAudioDecoder
		{
		public:
			bool Adopt(const uint8_t* data, size_t size, std::vector<uint8_t>&& owned, const std::string& name)
			{
				owned_ = std::move(owned);
				data_ = owned_.empty() ? data : owned_.data();
				size_ = owned_.empty() ? size : owned_.size();
				name_ = name;
				return data_ != nullptr && size_ > 0;
			}

			int64_t GetFrameOffset() const override { return cursor_; }
			bool IsSeekable() const override { return true; }

		protected:
			std::vector<uint8_t> owned_;
			const uint8_t* data_ = nullptr;
			size_t size_ = 0;
			std::string name_;
			int64_t cursor_ = 0;
		};

		// WAV

		constexpr uint16_t kWaveFormatPCM = 0x0001;
		constexpr uint16_t kWaveFormatMsAdpcm = 0x0002;
		constexpr uint16_t kWaveFormatFloat = 0x0003;
		constexpr uint16_t kWaveFormatImaAdpcm = 0x0011;
		constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

		// MS-ADPCM's default predictor set. A file may ship its own in the fmt chunk
		// extension; these seven are what every encoder actually writes.
		constexpr int16_t kMsDefaultCoef[7][2] = {
			{ 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 },
			{ 240, 0 }, { 460, -208 }, { 392, -232 },
		};

		constexpr int kMsAdaptTable[16] = {
			230, 230, 230, 230, 307, 409, 512, 614,
			768, 614, 512, 409, 307, 230, 230, 230,
		};

		constexpr int kImaIndexTable[16] = {
			-1, -1, -1, -1, 2, 4, 6, 8,
			-1, -1, -1, -1, 2, 4, 6, 8,
		};

		constexpr int kImaStepTable[89] = {
			7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
			34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
			157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
			724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
			3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
			15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
		};

		class WavDecoder final : public DecoderBase
		{
		public:
			bool Open()
			{
				if (size_ < 12 || !tag(data_, "RIFF") || !tag(data_ + 8, "WAVE"))
					return false;

				const uint8_t* fmt = nullptr;
				uint32_t fmtSize = 0;

				// Walk the chunk list. Chunks are word-aligned, so an odd size is
				// followed by a pad byte that the size field does not count.
				size_t pos = 12;
				while (pos + 8 <= size_)
				{
					const uint8_t* id = data_ + pos;
					const uint32_t chunkSize = rd32(data_ + pos + 4);
					const uint8_t* body = data_ + pos + 8;
					if (pos + 8 + (size_t)chunkSize > size_)
						break; // truncated: keep whatever complete chunks we already have

					if (tag(id, "fmt "))
					{
						fmt = body;
						fmtSize = chunkSize;
					}
					else if (tag(id, "data"))
					{
						dataOffset_ = pos + 8;
						dataSize_ = (size_t)chunkSize;
					}
					pos += 8 + (size_t)chunkSize + ((chunkSize & 1u) ? 1u : 0u);
				}

				if (fmt == nullptr || fmtSize < 16 || dataSize_ == 0)
					return false;

				format_ = rd16(fmt);
				channels_ = (int)rd16(fmt + 2);
				sampleRate_ = (int)rd32(fmt + 4);
				blockAlign_ = (int)rd16(fmt + 12);
				bits_ = (int)rd16(fmt + 14);
				if (format_ == kWaveFormatExtensible && fmtSize >= 40)
				{
					// The real format tag is the first two bytes of the sub-format GUID.
					format_ = rd16(fmt + 24);
				}
				if (channels_ <= 0 || channels_ > 8 || sampleRate_ <= 0)
					return false;

				switch (format_)
				{
				case kWaveFormatPCM:
				case kWaveFormatFloat:
					return OpenPcm();
				case kWaveFormatMsAdpcm:
					return OpenMsAdpcm(fmt, fmtSize);
				case kWaveFormatImaAdpcm:
					return OpenImaAdpcm(fmt, fmtSize);
				default:
					wilog_warning("stAudioDecoder: \"%s\" uses unsupported WAV format tag 0x%04X.",
						name_.c_str(), (unsigned)format_);
					return false;
				}
			}

			AudioFormat GetFormat() const override { return AudioFormat::Wav; }
			int GetChannels() const override { return channels_; }
			int GetSampleRate() const override { return sampleRate_; }
			int64_t GetTotalFrames() const override { return totalFrames_; }

			int ReadFrames(float* out, int frames) override
			{
				if (out == nullptr || frames <= 0)
					return 0;
				frames = (int)std::min<int64_t>(frames, totalFrames_ - cursor_);
				if (frames <= 0)
					return 0;
				const int produced = IsAdpcm() ? ReadAdpcm(out, frames) : ReadPcm(out, frames);
				cursor_ += produced;
				return produced;
			}

			bool Seek(int64_t frame) override
			{
				if (frame < 0 || frame > totalFrames_)
					return false;
				cursor_ = frame;
				if (IsAdpcm())
				{
					// Invalidate the block cache; the next read decodes the block the
					// new cursor lands in.
					cachedBlock_ = -1;
				}
				return true;
			}

		private:
			bool IsAdpcm() const { return format_ == kWaveFormatMsAdpcm || format_ == kWaveFormatImaAdpcm; }

			bool OpenPcm()
			{
				if (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32)
				{
					wilog_warning("stAudioDecoder: \"%s\" has unsupported WAV bit depth %d.", name_.c_str(), bits_);
					return false;
				}
				if (format_ == kWaveFormatFloat && bits_ != 32)
				{
					wilog_warning("stAudioDecoder: \"%s\" is float WAV at %d bits; only 32 is supported.",
						name_.c_str(), bits_);
					return false;
				}
				bytesPerFrame_ = channels_ * (bits_ / 8);
				if (bytesPerFrame_ <= 0)
					return false;
				totalFrames_ = (int64_t)(dataSize_ / (size_t)bytesPerFrame_);
				return totalFrames_ > 0;
			}

			bool OpenMsAdpcm(const uint8_t* fmt, uint32_t fmtSize)
			{
				if (blockAlign_ < 7 * channels_)
					return false;

				// cbSize >= 4 means the extension carries wSamplesPerBlock and the
				// coefficient table; encoders that omit it leave the block layout
				// derivable from blockAlign alone.
				const uint32_t cbSize = (fmtSize >= 18) ? rd16(fmt + 16) : 0;
				if (fmtSize >= 20 && cbSize >= 2)
					samplesPerBlock_ = (int)rd16(fmt + 18);
				else
					samplesPerBlock_ = ((blockAlign_ - 7 * channels_) * 2) / channels_ + 2;

				int numCoef = 7;
				if (fmtSize >= 22 && cbSize >= 4)
					numCoef = (int)rd16(fmt + 20);
				if (numCoef > 0 && numCoef <= 64 && fmtSize >= 22u + (uint32_t)numCoef * 4u)
				{
					coefs_.resize((size_t)numCoef);
					for (int i = 0; i < numCoef; ++i)
					{
						coefs_[(size_t)i][0] = (int16_t)rd16(fmt + 22 + i * 4);
						coefs_[(size_t)i][1] = (int16_t)rd16(fmt + 24 + i * 4);
					}
				}
				else
				{
					coefs_.resize(7);
					for (int i = 0; i < 7; ++i)
					{
						coefs_[(size_t)i][0] = kMsDefaultCoef[i][0];
						coefs_[(size_t)i][1] = kMsDefaultCoef[i][1];
					}
				}
				return FinishAdpcm();
			}

			bool OpenImaAdpcm(const uint8_t* fmt, uint32_t fmtSize)
			{
				if (blockAlign_ < 4 * channels_)
					return false;
				const uint32_t cbSize = (fmtSize >= 18) ? rd16(fmt + 16) : 0;
				if (fmtSize >= 20 && cbSize >= 2)
					samplesPerBlock_ = (int)rd16(fmt + 18);
				else
					samplesPerBlock_ = ((blockAlign_ - 4 * channels_) * 2) / channels_ + 1;
				return FinishAdpcm();
			}

			bool FinishAdpcm()
			{
				if (samplesPerBlock_ <= 0 || blockAlign_ <= 0)
					return false;
				blockCount_ = (int64_t)(dataSize_ / (size_t)blockAlign_);
				const size_t tailBytes = dataSize_ % (size_t)blockAlign_;
				totalFrames_ = blockCount_ * (int64_t)samplesPerBlock_;

				// A trailing short block is legal and common - encoders pad to the
				// block size only when the muxer demands it. Decode it to find out how
				// many frames it really carries rather than assuming a full block.
				if (tailBytes > (size_t)(format_ == kWaveFormatMsAdpcm ? 7 * channels_ : 4 * channels_))
				{
					blockScratch_.assign((size_t)samplesPerBlock_ * (size_t)channels_, 0.0f);
					const int tailFrames = DecodeBlock(data_ + dataOffset_ + (size_t)blockCount_ * (size_t)blockAlign_,
						tailBytes, blockScratch_.data());
					if (tailFrames > 0)
					{
						tailFrames_ = tailFrames;
						totalFrames_ += tailFrames;
						++blockCount_;
					}
				}
				blockScratch_.assign((size_t)samplesPerBlock_ * (size_t)channels_, 0.0f);
				cachedBlock_ = -1;
				return totalFrames_ > 0;
			}

			int ReadPcm(float* out, int frames)
			{
				const uint8_t* base = data_ + dataOffset_ + (size_t)cursor_ * (size_t)bytesPerFrame_;
				const size_t total = (size_t)frames * (size_t)channels_;
				switch (bits_)
				{
				case 8: // WAV 8-bit is UNSIGNED, unlike every other width
					for (size_t i = 0; i < total; ++i)
						out[i] = ((float)base[i] - 128.0f) / 128.0f;
					break;
				case 16:
					for (size_t i = 0; i < total; ++i)
						out[i] = (float)(int16_t)rd16(base + i * 2) / 32768.0f;
					break;
				case 24:
					for (size_t i = 0; i < total; ++i)
					{
						const uint8_t* s = base + i * 3;
						int32_t v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16));
						if (v & 0x800000) v |= (int32_t)0xFF000000; // sign-extend 24 -> 32
						out[i] = (float)v / 8388608.0f;
					}
					break;
				case 32:
					if (format_ == kWaveFormatFloat)
					{
						for (size_t i = 0; i < total; ++i)
						{
							const uint32_t bitsLE = rd32(base + i * 4);
							float f; std::memcpy(&f, &bitsLE, sizeof(f));
							out[i] = f;
						}
					}
					else
					{
						for (size_t i = 0; i < total; ++i)
							out[i] = (float)(int32_t)rd32(base + i * 4) / 2147483648.0f;
					}
					break;
				default:
					return 0;
				}
				return frames;
			}

			int ReadAdpcm(float* out, int frames)
			{
				int written = 0;
				while (written < frames)
				{
					const int64_t frame = cursor_ + written;
					const int64_t block = frame / (int64_t)samplesPerBlock_;
					const int offsetInBlock = (int)(frame % (int64_t)samplesPerBlock_);
					if (block >= blockCount_)
						break;
					if (block != cachedBlock_ && !DecodeBlockIntoCache(block))
						break;

					const int framesInBlock = (block == blockCount_ - 1 && tailFrames_ > 0)
						? tailFrames_ : samplesPerBlock_;
					const int take = std::min(frames - written, framesInBlock - offsetInBlock);
					if (take <= 0)
						break;
					std::memcpy(out + (size_t)written * (size_t)channels_,
						blockScratch_.data() + (size_t)offsetInBlock * (size_t)channels_,
						(size_t)take * (size_t)channels_ * sizeof(float));
					written += take;
				}
				return written;
			}

			bool DecodeBlockIntoCache(int64_t block)
			{
				const size_t offset = dataOffset_ + (size_t)block * (size_t)blockAlign_;
				if (offset >= size_)
					return false;
				const size_t avail = std::min((size_t)blockAlign_, size_ - offset);
				std::fill(blockScratch_.begin(), blockScratch_.end(), 0.0f);
				if (DecodeBlock(data_ + offset, avail, blockScratch_.data()) <= 0)
					return false;
				cachedBlock_ = block;
				return true;
			}

			int DecodeBlock(const uint8_t* p, size_t avail, float* out) const
			{
				return format_ == kWaveFormatMsAdpcm ? DecodeMsBlock(p, avail, out)
					: DecodeImaBlock(p, avail, out);
			}

			int DecodeMsBlock(const uint8_t* p, size_t avail, float* out) const
			{
				const int ch = channels_;
				if (avail < (size_t)(7 * ch))
					return 0;

				int predictor[8], delta[8], s1[8], s2[8];
				const uint8_t* q = p;
				for (int c = 0; c < ch; ++c) predictor[c] = ClampInt((int)*q++, 0, (int)coefs_.size() - 1);
				for (int c = 0; c < ch; ++c) { delta[c] = (int16_t)rd16(q); q += 2; }
				for (int c = 0; c < ch; ++c) { s1[c] = (int16_t)rd16(q); q += 2; }
				for (int c = 0; c < ch; ++c) { s2[c] = (int16_t)rd16(q); q += 2; }
				avail -= (size_t)(7 * ch);

				// The block header IS the first two frames, older one first.
				for (int c = 0; c < ch; ++c) out[c] = (float)s2[c] / 32768.0f;
				for (int c = 0; c < ch; ++c) out[(size_t)ch + (size_t)c] = (float)s1[c] / 32768.0f;

				const int nibblesAvailable = (int)std::min<size_t>(avail * 2, (size_t)INT32_MAX);
				const int maxFrames = std::min(samplesPerBlock_ - 2, nibblesAvailable / ch);
				for (int i = 0; i < maxFrames; ++i)
				{
					for (int c = 0; c < ch; ++c)
					{
						const int nibbleIndex = i * ch + c;
						const uint8_t byte = q[nibbleIndex / 2];
						const int nibble = (nibbleIndex & 1) ? (byte & 0x0F) : (byte >> 4);
						const int signedNibble = (nibble >= 8) ? nibble - 16 : nibble;

						int predicted = (s1[c] * (int)coefs_[(size_t)predictor[c]][0]
							+ s2[c] * (int)coefs_[(size_t)predictor[c]][1]) / 256;
						predicted += signedNibble * delta[c];
						predicted = ClampInt(predicted, -32768, 32767);

						s2[c] = s1[c];
						s1[c] = predicted;

						const int nextDelta = (kMsAdaptTable[nibble] * delta[c]) / 256;
						delta[c] = nextDelta < 16 ? 16 : nextDelta;

						out[(size_t)(i + 2) * (size_t)ch + (size_t)c] = (float)predicted / 32768.0f;
					}
				}
				return 2 + maxFrames;
			}

			int DecodeImaBlock(const uint8_t* p, size_t avail, float* out) const
			{
				const int ch = channels_;
				if (avail < (size_t)(4 * ch))
					return 0;

				int predictor[8], index[8];
				const uint8_t* q = p;
				for (int c = 0; c < ch; ++c)
				{
					predictor[c] = (int16_t)rd16(q);
					index[c] = ClampInt((int)q[2], 0, 88);
					q += 4; // the fourth byte is reserved and always zero
					out[(size_t)c] = (float)predictor[c] / 32768.0f;
				}
				avail -= (size_t)(4 * ch);

				// The data is 4-byte words, round-robin across channels: eight samples
				// of channel 0, eight of channel 1, and so on.
				const size_t groupBytes = (size_t)4 * (size_t)ch;
				const int groups = (int)(avail / groupBytes);
				int frames = 1;
				for (int g = 0; g < groups; ++g)
				{
					const uint8_t* group = q + (size_t)g * groupBytes;
					for (int c = 0; c < ch; ++c)
					{
						const uint8_t* word = group + (size_t)c * 4;
						for (int n = 0; n < 8; ++n)
						{
							const int frameIndex = 1 + g * 8 + n;
							if (frameIndex >= samplesPerBlock_)
								break;
							const uint8_t byte = word[n / 2];
							// IMA packs the LOW nibble first, the opposite of MS-ADPCM.
							const int nibble = (n & 1) ? (byte >> 4) : (byte & 0x0F);

							// One multiply and one shift, not four shifted adds. The two
							// forms differ only in where they truncate, but every decoder
							// a file is likely to have been encoded against (ffmpeg, the
							// dr_wav family) rounds this way, and matching it makes the
							// output bit-identical rather than ~50 LSB off.
							const int step = kImaStepTable[index[c]];
							int diff = ((2 * (nibble & 7) + 1) * step) >> 3;
							if (nibble & 8) diff = -diff;

							predictor[c] = ClampInt(predictor[c] + diff, -32768, 32767);
							index[c] = ClampInt(index[c] + kImaIndexTable[nibble], 0, 88);
							out[(size_t)frameIndex * (size_t)ch + (size_t)c] = (float)predictor[c] / 32768.0f;
						}
					}
					frames = std::min(samplesPerBlock_, 1 + (g + 1) * 8);
				}
				return frames;
			}

			uint16_t format_ = 0;
			int channels_ = 0;
			int sampleRate_ = 0;
			int bits_ = 0;
			int blockAlign_ = 0;
			int bytesPerFrame_ = 0;
			size_t dataOffset_ = 0;
			size_t dataSize_ = 0;
			int64_t totalFrames_ = 0;

			// ADPCM only
			int samplesPerBlock_ = 0;
			int64_t blockCount_ = 0;
			int tailFrames_ = 0;              // frames in a short final block, 0 if none
			int64_t cachedBlock_ = -1;
			std::vector<float> blockScratch_; // one decoded block, interleaved
			std::vector<std::array<int16_t, 2>> coefs_;
		};

		// Ogg Vorbis

		class VorbisDecoder final : public DecoderBase
		{
		public:
			~VorbisDecoder() override
			{
				if (vorbis_ != nullptr)
					stb_vorbis_close(vorbis_);
			}

			bool Open()
			{
				int error = 0;
				vorbis_ = stb_vorbis_open_memory(data_, (int)size_, &error, nullptr);
				if (vorbis_ == nullptr)
				{
					wilog_warning("stAudioDecoder: \"%s\" is not decodable Ogg Vorbis (stb_vorbis error %d).",
						name_.c_str(), error);
					return false;
				}
				const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
				channels_ = (int)info.channels;
				sampleRate_ = (int)info.sample_rate;
				totalFrames_ = (int64_t)stb_vorbis_stream_length_in_samples(vorbis_);
				return channels_ > 0 && sampleRate_ > 0;
			}

			AudioFormat GetFormat() const override { return AudioFormat::OggVorbis; }
			int GetChannels() const override { return channels_; }
			int GetSampleRate() const override { return sampleRate_; }
			int64_t GetTotalFrames() const override { return totalFrames_; }

			int ReadFrames(float* out, int frames) override
			{
				if (vorbis_ == nullptr || out == nullptr || frames <= 0)
					return 0;
				// stb_vorbis counts FLOATS here, not frames, and returns frames.
				const int produced = stb_vorbis_get_samples_float_interleaved(
					vorbis_, channels_, out, frames * channels_);
				if (produced > 0)
					cursor_ += produced;
				return produced;
			}

			bool Seek(int64_t frame) override
			{
				if (vorbis_ == nullptr || frame < 0)
					return false;
				if (stb_vorbis_seek(vorbis_, (unsigned int)frame) == 0)
					return false;
				cursor_ = frame;
				return true;
			}

		private:
			stb_vorbis* vorbis_ = nullptr;
			int channels_ = 0;
			int sampleRate_ = 0;
			int64_t totalFrames_ = 0;
		};

		// QOA

		class QoaDecoder final : public DecoderBase
		{
		public:
			bool Open()
			{
				if (qoa_decode_header(data_, (int)std::min<size_t>(size_, INT32_MAX), &desc_) != 8)
					return false;

				channels_ = (int)desc_.channels;
				sampleRate_ = (int)desc_.samplerate;
				totalFrames_ = (int64_t)desc_.samples;
				// Every frame but the last holds exactly QOA_FRAME_LEN samples in
				// exactly QOA_FRAME_SIZE bytes, so a seek is arithmetic, not a scan.
				frameBytes_ = (size_t)QOA_FRAME_SIZE(desc_.channels, QOA_SLICES_PER_FRAME);
				frameScratch_.assign((size_t)QOA_FRAME_LEN * (size_t)channels_, 0);
				return channels_ > 0 && sampleRate_ > 0 && totalFrames_ > 0;
			}

			AudioFormat GetFormat() const override { return AudioFormat::Qoa; }
			int GetChannels() const override { return channels_; }
			int GetSampleRate() const override { return sampleRate_; }
			int64_t GetTotalFrames() const override { return totalFrames_; }

			int ReadFrames(float* out, int frames) override
			{
				if (out == nullptr || frames <= 0)
					return 0;
				frames = (int)std::min<int64_t>(frames, totalFrames_ - cursor_);
				if (frames <= 0)
					return 0;

				int written = 0;
				while (written < frames)
				{
					const int64_t qoaFrame = (cursor_ + written) / QOA_FRAME_LEN;
					const int offsetInFrame = (int)((cursor_ + written) % QOA_FRAME_LEN);
					if (qoaFrame != cachedFrame_ && !DecodeQoaFrame(qoaFrame))
						break;

					const int take = std::min(frames - written, cachedFrameLen_ - offsetInFrame);
					if (take <= 0)
						break;
					const short* src = frameScratch_.data() + (size_t)offsetInFrame * (size_t)channels_;
					float* dst = out + (size_t)written * (size_t)channels_;
					const size_t total = (size_t)take * (size_t)channels_;
					for (size_t i = 0; i < total; ++i)
						dst[i] = (float)src[i] / 32768.0f;
					written += take;
				}
				cursor_ += written;
				return written;
			}

			bool Seek(int64_t frame) override
			{
				if (frame < 0 || frame > totalFrames_)
					return false;
				cursor_ = frame;
				return true;
			}

		private:
			bool DecodeQoaFrame(int64_t index)
			{
				const size_t offset = 8 + (size_t)index * frameBytes_;
				if (offset >= size_)
					return false;
				unsigned int frameLen = 0;
				// The decoder's LMS state travels in the frame header, so decoding a
				// frame never depends on the one before it.
				const unsigned int consumed = qoa_decode_frame(data_ + offset,
					(unsigned int)std::min<size_t>(size_ - offset, UINT32_MAX),
					&desc_, frameScratch_.data(), &frameLen);
				if (consumed == 0 || frameLen == 0)
					return false;
				cachedFrame_ = index;
				cachedFrameLen_ = (int)frameLen;
				return true;
			}

			qoa_desc desc_{};
			int channels_ = 0;
			int sampleRate_ = 0;
			int64_t totalFrames_ = 0;
			size_t frameBytes_ = 0;
			int64_t cachedFrame_ = -1;
			int cachedFrameLen_ = 0;
			std::vector<short> frameScratch_;
		};

		// Opus (Ogg-encapsulated)

#if defined(SIMTARY_HAS_OPUS)

		// Opus always decodes at 48 kHz. The codec has no other output rate, and the
		// engine mixes at 48 kHz, so an Opus asset is the one format that never
		// touches the resampler.
		constexpr int kOpusRate = 48000;
		// 120 ms, the longest packet Opus can carry.
		constexpr int kOpusMaxPacketFrames = 5760;
		// Run-up after a seek: decode this many frames before the target and throw
		// them away, so the decoder state has re-converged by the time samples are
		// handed out. RFC 7845 asks for at least 80 ms. Measured against a
		// straight-through decode of the same file, 80 ms still left ~-22 dB of error
		// at the seek point - SILK's long-term predictor re-converges slowly - while
		// 200 ms takes it to about -57 dB, which is inaudible. The cost is ten more
		// packets decoded and discarded per seek: microseconds.
		constexpr int kOpusPrerollFrames = 9600;

		class OggOpusDecoder final : public DecoderBase
		{
		public:
			~OggOpusDecoder() override
			{
				if (decoder_ != nullptr)
					opus_multistream_decoder_destroy(decoder_);
			}

			bool Open()
			{
				if (!ScanOgg())
					return false;
				if (packets_.empty())
				{
					wilog_warning("stAudioDecoder: \"%s\" is Ogg-Opus with no audio packets.", name_.c_str());
					return false;
				}

				int error = 0;
				decoder_ = opus_multistream_decoder_create(kOpusRate, channels_,
					streamCount_, coupledCount_, mapping_.data(), &error);
				if (decoder_ == nullptr || error != OPUS_OK)
				{
					wilog_warning("stAudioDecoder: \"%s\" - opus_multistream_decoder_create failed (%s).",
						name_.c_str(), opus_strerror(error));
					return false;
				}

				packetScratch_.assign((size_t)kOpusMaxPacketFrames * (size_t)channels_, 0.0f);
				Rewind();
				return true;
			}

			AudioFormat GetFormat() const override { return AudioFormat::Opus; }
			int GetChannels() const override { return channels_; }
			int GetSampleRate() const override { return kOpusRate; }
			int64_t GetTotalFrames() const override { return totalFrames_; }

			int ReadFrames(float* out, int frames) override
			{
				if (decoder_ == nullptr || out == nullptr || frames <= 0)
					return 0;
				frames = (int)std::min<int64_t>(frames, totalFrames_ - cursor_);
				if (frames <= 0)
					return 0;

				int written = 0;
				while (written < frames)
				{
					if (pending_ <= 0 && !DecodeNextPacket())
						break;
					const int take = std::min(frames - written, pending_);
					std::memcpy(out + (size_t)written * (size_t)channels_,
						packetScratch_.data() + (size_t)pendingOffset_ * (size_t)channels_,
						(size_t)take * (size_t)channels_ * sizeof(float));
					pendingOffset_ += take;
					pending_ -= take;
					written += take;
				}
				cursor_ += written;
				return written;
			}

			bool Seek(int64_t frame) override
			{
				if (decoder_ == nullptr || frame < 0 || frame > totalFrames_)
					return false;

				// Packet start frames are in RAW decoder output, which still carries
				// the pre-skip the encoder padded the stream with.
				const int64_t targetRaw = frame + preSkip_;

				size_t index = 0;
				while (index + 1 < packets_.size() && packets_[index + 1].startFrame <= targetRaw)
					++index;
				// Rewind far enough that the decoder has converged by the target.
				while (index > 0 && packets_[index].startFrame > targetRaw - kOpusPrerollFrames)
					--index;

				opus_multistream_decoder_ctl(decoder_, OPUS_RESET_STATE);
				nextPacket_ = index;
				pending_ = 0;
				pendingOffset_ = 0;
				skipFrames_ = (int)std::max<int64_t>(0, targetRaw - packets_[index].startFrame);
				cursor_ = frame;
				return true;
			}

		private:
			struct Packet
			{
				uint64_t offset = 0;   // into data_, or into spill_ when spill >= 0
				uint32_t size = 0;
				int64_t startFrame = 0; // raw decoder frames before this packet
				int32_t spill = -1;     // index into spill_ for a page-spanning packet
			};

			void Rewind()
			{
				opus_multistream_decoder_ctl(decoder_, OPUS_RESET_STATE);
				nextPacket_ = 0;
				pending_ = 0;
				pendingOffset_ = 0;
				skipFrames_ = preSkip_;
				cursor_ = 0;
			}

			const uint8_t* PacketData(const Packet& p) const
			{
				return (p.spill >= 0) ? spill_[(size_t)p.spill].data() : data_ + p.offset;
			}

			bool DecodeNextPacket()
			{
				while (nextPacket_ < packets_.size())
				{
					const Packet& p = packets_[nextPacket_++];
					const int decoded = opus_multistream_decode_float(decoder_, PacketData(p), (opus_int32)p.size,
						packetScratch_.data(), kOpusMaxPacketFrames, 0);
					if (decoded < 0)
					{
						wilog_warning("stAudioDecoder: \"%s\" - opus decode error (%s); skipping a packet.",
							name_.c_str(), opus_strerror(decoded));
						continue;
					}
					// Output gain is part of the FILE, not a mixer setting: the encoder
					// wrote it into OpusHead and every decoder is expected to apply it.
					if (outputGain_ != 1.0f)
					{
						const size_t total = (size_t)decoded * (size_t)channels_;
						for (size_t i = 0; i < total; ++i)
							packetScratch_[i] *= outputGain_;
					}

					// Pre-skip at the head of the stream, and the run-up after a seek,
					// are both "decode it, then throw it away".
					const int skip = std::min(skipFrames_, decoded);
					skipFrames_ -= skip;
					pendingOffset_ = skip;
					pending_ = decoded - skip;
					if (pending_ > 0)
						return true;
				}
				return false;
			}

			// Walk the Ogg pages once: find the Opus logical stream, read OpusHead,
			// and index every audio packet with the frame it starts at. Indexing up
			// front is what makes seeking exact - granule positions are per-page and
			// would only get us to the nearest page.
			bool ScanOgg()
			{
				std::vector<uint8_t> partial;

				size_t pos = 0;
				while (pos + 27 <= size_)
				{
					if (!tag(data_ + pos, "OggS"))
					{
						++pos; // resync rather than give up: a stray byte is recoverable
						continue;
					}
					const uint8_t headerType = data_[pos + 5];
					const uint64_t granule = rd64(data_ + pos + 6);
					const uint32_t pageSerial = rd32(data_ + pos + 14);
					const int segmentCount = (int)data_[pos + 26];
					if (pos + 27 + (size_t)segmentCount > size_)
						break;

					const uint8_t* segments = data_ + pos + 27;
					size_t bodySize = 0;
					for (int i = 0; i < segmentCount; ++i)
						bodySize += segments[i];
					const size_t bodyOffset = pos + 27 + (size_t)segmentCount;
					if (bodyOffset + bodySize > size_)
						break;

					const size_t pageEnd = bodyOffset + bodySize;
					if (haveHead_ && pageSerial != serial_)
					{
						pos = pageEnd; // a different logical stream (cover art, a second track)
						continue;
					}

					size_t runStart = bodyOffset;
					size_t runLength = 0;
					bool firstInPage = true;
					const bool continuation = (headerType & 0x01) != 0;

					for (int i = 0; i < segmentCount; ++i)
					{
						runLength += segments[i];
						if (segments[i] == 255)
							continue; // the packet carries on into the next segment

						if (firstInPage && continuation && !partial.empty())
						{
							partial.insert(partial.end(), data_ + runStart, data_ + runStart + runLength);
							AcceptSpilledPacket(partial, pageSerial);
							partial.clear();
						}
						else
						{
							AcceptPacket(data_ + runStart, (uint32_t)runLength, runStart, pageSerial);
						}
						firstInPage = false;
						runStart += runLength;
						runLength = 0;
					}

					// Trailing 255-segments mean the packet continues on the next page.
					if (runLength > 0)
					{
						if (firstInPage && continuation && !partial.empty())
							partial.insert(partial.end(), data_ + runStart, data_ + runStart + runLength);
						else
							partial.assign(data_ + runStart, data_ + runStart + runLength);
					}
					else if (!firstInPage)
					{
						partial.clear();
					}

					// -1 marks a page whose packet has not finished; anything else is the
					// running output count, and the last one is where the stream ends.
					if (granule != UINT64_MAX)
						lastGranule_ = (int64_t)granule;

					pos = pageEnd;
				}

				totalFrames_ = std::max<int64_t>(0, rawFrames_ - preSkip_);

				// An encoder pads the final packet out to a whole frame, so summing packet
				// durations overshoots by up to 120 ms of silence. The granule position on
				// the last page counts output samples INCLUDING the pre-skip, which is
				// exactly where the real audio stops.
				if (lastGranule_ >= 0)
				{
					const int64_t byGranule = lastGranule_ - (int64_t)preSkip_;
					if (byGranule > 0 && byGranule < totalFrames_)
						totalFrames_ = byGranule;
				}
				return haveHead_;
			}

			// A packet still sitting contiguously inside `data_` - the common case, and
			// the one that costs nothing but an index entry.
			void AcceptPacket(const uint8_t* packet, uint32_t length, size_t offset, uint32_t pageSerial)
			{
				if (!ClassifyPacket(packet, length, pageSerial))
					return;
				Packet entry;
				entry.size = length;
				entry.offset = (uint64_t)offset;
				entry.startFrame = rawFrames_;
				packets_.push_back(entry);
				rawFrames_ += lastPacketFrames_;
			}

			// A packet that spanned a page boundary, so it had to be reassembled into a
			// buffer of its own. Rare enough that keeping a copy is cheaper than
			// teaching the read path about discontiguous packets.
			void AcceptSpilledPacket(const std::vector<uint8_t>& packet, uint32_t pageSerial)
			{
				if (!ClassifyPacket(packet.data(), (uint32_t)packet.size(), pageSerial))
					return;
				Packet entry;
				entry.size = (uint32_t)packet.size();
				entry.spill = (int32_t)spill_.size();
				entry.startFrame = rawFrames_;
				spill_.push_back(packet);
				packets_.push_back(entry);
				rawFrames_ += lastPacketFrames_;
			}

			// True when the packet is audio worth indexing. The first two packets of an
			// Opus stream are OpusHead and OpusTags; this consumes those and leaves an
			// audio packet's duration in `lastPacketFrames_`.
			bool ClassifyPacket(const uint8_t* packet, uint32_t length, uint32_t pageSerial)
			{
				lastPacketFrames_ = 0;
				if (length == 0)
					return false;

				if (!haveHead_)
				{
					// Not the identification header: another logical stream's first page.
					if (length < 19 || !tag8(packet, "OpusHead") || !ParseOpusHead(packet, length))
						return false;
					serial_ = pageSerial;
					haveHead_ = true;
					return false;
				}
				if (!haveTags_)
				{
					haveTags_ = true;
					if (length >= 8 && tag8(packet, "OpusTags"))
						return false;
					// Some muxers omit OpusTags; fall through and treat this as audio.
				}

				const int frames = opus_packet_get_nb_samples(packet, (opus_int32)length, kOpusRate);
				if (frames <= 0)
					return false; // a malformed packet costs its own duration, nothing more
				lastPacketFrames_ = frames;
				return true;
			}

			bool ParseOpusHead(const uint8_t* head, uint32_t length)
			{
				channels_ = (int)head[9];
				preSkip_ = (int)rd16(head + 10);
				const int family = (int)head[18];
				if (channels_ <= 0 || channels_ > 255)
					return false;

				mapping_.assign((size_t)channels_, 0);
				if (family == 0)
				{
					if (channels_ > 2)
						return false; // family 0 is mono or stereo by definition
					streamCount_ = 1;
					coupledCount_ = channels_ - 1;
					for (int c = 0; c < channels_; ++c)
						mapping_[(size_t)c] = (uint8_t)c;
				}
				else
				{
					if (length < 21u + (uint32_t)channels_)
						return false;
					streamCount_ = (int)head[19];
					coupledCount_ = (int)head[20];
					if (streamCount_ <= 0 || coupledCount_ < 0 || coupledCount_ > streamCount_)
						return false;
					for (int c = 0; c < channels_; ++c)
						mapping_[(size_t)c] = head[21 + c];
				}

				// Output gain is Q7.8 dB and is part of the file, not a mixer setting:
				// the encoder put it there and every decoder is expected to apply it.
				const int16_t gainQ8 = (int16_t)rd16(head + 16);
				outputGain_ = std::pow(10.0f, (float)gainQ8 / (20.0f * 256.0f));
				return true;
			}

			OpusMSDecoder* decoder_ = nullptr;
			int channels_ = 0;
			int streamCount_ = 0;
			int coupledCount_ = 0;
			int preSkip_ = 0;
			float outputGain_ = 1.0f;
			int64_t totalFrames_ = 0;
			std::vector<uint8_t> mapping_;
			std::vector<Packet> packets_;
			std::vector<std::vector<uint8_t>> spill_; // page-spanning packets, reassembled
			std::vector<float> packetScratch_;
			size_t nextPacket_ = 0;
			int pending_ = 0;
			int pendingOffset_ = 0;
			int skipFrames_ = 0;

			// Scan-time state. Only ScanOgg and its helpers touch these; they are
			// members rather than locals so the packet walk does not have to thread
			// eight references through every call.
			bool haveHead_ = false;
			bool haveTags_ = false;
			uint32_t serial_ = 0;
			int64_t rawFrames_ = 0;
			int lastPacketFrames_ = 0;
			int64_t lastGranule_ = -1;
		};

#endif // SIMTARY_HAS_OPUS

		// Ogg carries both Vorbis and Opus, so "OggS" alone does not identify the
		// codec: the first page's body starts with the codec's own identification
		// header, and that is what separates them.
		AudioFormat SniffOgg(const uint8_t* data, size_t size)
		{
			if (size < 28)
				return AudioFormat::Unknown;
			const int segmentCount = (int)data[26];
			const size_t bodyOffset = 27 + (size_t)segmentCount;
			if (bodyOffset + 8 > size)
				return AudioFormat::Unknown;
			if (tag8(data + bodyOffset, "OpusHead"))
				return AudioFormat::Opus;
			// Vorbis identification header: packet type 0x01 then "vorbis".
			if (bodyOffset + 7 <= size && data[bodyOffset] == 0x01
				&& std::memcmp(data + bodyOffset + 1, "vorbis", 6) == 0)
				return AudioFormat::OggVorbis;
			return AudioFormat::Unknown;
		}

		AudioDecoderPtr Build(const uint8_t* data, size_t size, std::vector<uint8_t>&& owned,
			const std::string& name)
		{
			// Sniff against whichever block is live: `owned` when the caller handed
			// the bytes over, the borrowed pointer otherwise.
			const uint8_t* probe = owned.empty() ? data : owned.data();
			const size_t probeSize = owned.empty() ? size : owned.size();
			if (probe == nullptr || probeSize == 0)
			{
				wilog_warning("stAudioDecoder: empty data for \"%s\".", name.c_str());
				return {};
			}

			const AudioFormat format = SniffAudioFormat(probe, probeSize);
			switch (format)
			{
			case AudioFormat::Wav:
			{
				auto decoder = std::make_unique<WavDecoder>();
				if (decoder->Adopt(data, size, std::move(owned), name) && decoder->Open())
					return decoder;
				break;
			}
			case AudioFormat::OggVorbis:
			{
				auto decoder = std::make_unique<VorbisDecoder>();
				if (decoder->Adopt(data, size, std::move(owned), name) && decoder->Open())
					return decoder;
				break;
			}
			case AudioFormat::Qoa:
			{
				auto decoder = std::make_unique<QoaDecoder>();
				if (decoder->Adopt(data, size, std::move(owned), name) && decoder->Open())
					return decoder;
				break;
			}
			case AudioFormat::Opus:
			{
#if defined(SIMTARY_HAS_OPUS)
				auto decoder = std::make_unique<OggOpusDecoder>();
				if (decoder->Adopt(data, size, std::move(owned), name) && decoder->Open())
					return decoder;
#else
				wilog_error("stAudioDecoder: \"%s\" is Ogg-Opus but the engine was built without "
					"libopus (SIMTARY_ENABLE_OPUS=OFF).", name.c_str());
				return {};
#endif
				break;
			}
			default:
				wilog_error("stAudioDecoder: \"%s\" is not WAV, Ogg Vorbis, QOA or Ogg-Opus (or is corrupt).",
					name.c_str());
				return {};
			}

			wilog_error("stAudioDecoder: \"%s\" looks like %s but failed to open.",
				name.c_str(), GetAudioFormatName(format));
			return {};
		}
	}

	const char* GetAudioFormatName(AudioFormat format)
	{
		switch (format)
		{
		case AudioFormat::Wav:       return "WAV";
		case AudioFormat::OggVorbis: return "Ogg Vorbis";
		case AudioFormat::Qoa:       return "QOA";
		case AudioFormat::Opus:      return "Opus";
		default:                     return "unknown";
		}
	}

	AudioFormat SniffAudioFormat(const uint8_t* data, size_t size)
	{
		if (data == nullptr || size < 12)
			return AudioFormat::Unknown;
		if (tag(data, "RIFF") && tag(data + 8, "WAVE"))
			return AudioFormat::Wav;
		if (tag(data, "qoaf"))
			return AudioFormat::Qoa;
		if (tag(data, "OggS"))
			return SniffOgg(data, size);
		return AudioFormat::Unknown;
	}

	bool IsAudioFormatAvailable(AudioFormat format)
	{
		switch (format)
		{
		case AudioFormat::Wav:
		case AudioFormat::OggVorbis:
		case AudioFormat::Qoa:
			return true;
		case AudioFormat::Opus:
#if defined(SIMTARY_HAS_OPUS)
			return true;
#else
			return false;
#endif
		default:
			return false;
		}
	}

	AudioDecoderPtr CreateAudioDecoder(const uint8_t* data, size_t size, const std::string& name)
	{
		return Build(data, size, {}, name);
	}

	AudioDecoderPtr CreateAudioDecoder(std::vector<uint8_t>&& bytes, const std::string& name)
	{
		return Build(nullptr, 0, std::move(bytes), name);
	}

	AudioDecoderPtr OpenAudioFile(const std::string& filename)
	{
		wi::vector<uint8_t> read;
		if (!wi::helper::FileRead(filename, read) || read.empty())
		{
			wilog_error("stAudioDecoder: could not read \"%s\".", filename.c_str());
			return {};
		}
		// The decoder outlives this call, so the bytes have to be handed over rather
		// than borrowed from a temporary.
		std::vector<uint8_t> bytes(read.begin(), read.end());
		return CreateAudioDecoder(std::move(bytes), filename);
	}
}
