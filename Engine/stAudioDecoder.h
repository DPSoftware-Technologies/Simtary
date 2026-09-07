#pragma once
// stAudioDecoder: incremental decoders, one per container the engine reads
//
// Everything that turns bytes into float samples lives behind IAudioDecoder. There
// are two consumers and they want opposite things:
//
//	stAudioClip.cpp   pulls the WHOLE stream in one go and keeps the PCM. Right for
//	                  a footstep, wrong for a five-minute track.
//	stAudioStream.cpp keeps the COMPRESSED bytes and pulls a block at a time on a
//	                  worker thread. A 4 MB Opus file stays 4 MB instead of becoming
//	                  the 340 MB of 48 kHz stereo float it decodes to.
//
// Both go through the same decoders, so a format is added once and both paths get
// it. A decoder never resamples and never downmixes: it reports the rate and channel
// count the file actually has, and the caller (the clip loader, or the stream
// player's resampler) is what reconciles that with the mix rate.
//
// "Streaming" here means streaming DECODE, not streaming IO. The compressed bytes
// are resident - read once, owned by the decoder - and only the PCM is produced on
// demand. That is the trade a game wants: no file handle held open for the whole
// level, no disk hitch inside the audio thread's deadline, and the memory saved is
// the 20-100x that compression already bought.
//
// formats
//
//	Wav        RIFF/WAVE. PCM 8/16/24/32-bit integer, 32-bit float, MS-ADPCM
//	           (0x0002) and IMA/DVI ADPCM (0x0011), plus WAVE_FORMAT_EXTENSIBLE
//	           wrapping any of those. ADPCM is 4:1 at near-zero decode cost, which
//	           is what large sound-effect banks want.
//	OggVorbis  via stb_vorbis (the copy FAudio already compiles).
//	Qoa        Quite OK Audio. Fixed 3.2:1, no bit reservoir, no entropy coding, so
//	           decoding is a few adds per sample and every frame is independently
//	           seekable. The middle ground between raw PCM and a real codec.
//	Opus       Ogg-encapsulated Opus, when the engine was built with libopus
//	           (SIMTARY_HAS_OPUS). Always decodes at 48 kHz - the codec has no other
//	           output rate - which is already the engine's mix rate, so the
//	           resampler never runs on an Opus asset.
//
// Thread safety: a decoder is NOT thread safe. One decoder, one thread; the stream
// player owns its decoder outright and never shares it with the audio thread.

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace st::audio
{
	enum class AudioFormat
	{
		Unknown,
		Wav,
		OggVorbis,
		Qoa,
		Opus,
	};

	// "WAV", "Ogg Vorbis", ... - for logs and tooling.
	const char* GetAudioFormatName(AudioFormat format);

	// Identify by magic bytes alone; extensions are never trusted. Returns Unknown
	// for anything that is not one of the containers above.
	AudioFormat SniffAudioFormat(const uint8_t* data, size_t size);

	// False for a format that exists in the enum but was compiled out (Opus without
	// libopus). Use it to fail an asset-pipeline check early instead of at load.
	bool IsAudioFormatAvailable(AudioFormat format);

	class IAudioDecoder
	{
	public:
		virtual ~IAudioDecoder() = default;

		virtual AudioFormat GetFormat() const = 0;
		virtual int GetChannels() const = 0;
		virtual int GetSampleRate() const = 0;

		// Total frames in the stream, or -1 when the container does not say. Frames,
		// not samples: a frame is one sample per channel.
		virtual int64_t GetTotalFrames() const = 0;

		// Decode up to `frames` frames into `interleaved` (frames * GetChannels()
		// floats, roughly [-1, 1]). Returns frames actually produced; a short read
		// means the stream ended, and 0 means it is done. Never blocks.
		virtual int ReadFrames(float* interleaved, int frames) = 0;

		// Move the read cursor. Returns false when the format cannot seek or the
		// target is out of range; the cursor is left where it was.
		virtual bool Seek(int64_t frame) = 0;

		// Every format here is seekable, but a decoder can decline (a truncated
		// index, a stream with no length). Check before offering scrubbing UI.
		virtual bool IsSeekable() const = 0;

		// Frames already handed out - the play cursor, in the stream's own rate.
		virtual int64_t GetFrameOffset() const = 0;
	};

	using AudioDecoderPtr = std::unique_ptr<IAudioDecoder>;

	// Decoder over bytes the CALLER owns. `data` must outlive the decoder - every
	// decoder here reads from the block lazily rather than copying it.
	AudioDecoderPtr CreateAudioDecoder(const uint8_t* data, size_t size,
		const std::string& name = "memory");

	// Decoder that TAKES the byte block. This is the streaming entry point: the
	// compressed bytes live inside the decoder for as long as it does.
	AudioDecoderPtr CreateAudioDecoder(std::vector<uint8_t>&& bytes,
		const std::string& name = "memory");

	// Read the file once, keep the compressed bytes, decode on demand. Returns null
	// on a missing file or an unrecognised container, having logged which.
	AudioDecoderPtr OpenAudioFile(const std::string& filename);
}
