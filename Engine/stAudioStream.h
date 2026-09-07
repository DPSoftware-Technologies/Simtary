#pragma once
// stAudioStream: play an asset without ever materialising its PCM
//
// An AudioClip decodes the whole file to float at load and keeps it. That is the
// right trade for a footstep and the wrong one for a soundtrack: a five-minute
// stereo track is ~110 MB of 48 kHz float, and the game touches 10 ms of it at a
// time. A StreamPlayer holds the COMPRESSED bytes instead - a few megabytes - and
// decodes one block ahead on its own worker thread.
//
//	AudioClip      decode once, keep the PCM.   Short, replayed, latency-critical.
//	StreamPlayer   keep the bytes, decode live. Long, played once through.
//
// It drives an ordinary Emitter, so everything the engine can do to an emitter
// applies: submix routing, volume, pitch is the one exception (the stream feeds the
// emitter's input buffer, which the mixer does not resample), and via GetEmitter()
// the full Steam Audio path - a radio in the world can stream a track and still be
// occluded by the wall it is behind.
//
// The emitter's input buffer is MONO, like every emitter in this engine, so a stereo
// asset is downmixed as it is decoded. That is not a streaming limitation - a clip
// on a 3D emitter is downmixed in exactly the same place, for the same reason: a
// point in space radiates one signal.
//
// threading
//	game thread   Open / Close / SetVolume / Seek. Never blocks on the decoder.
//	worker thread owns the decoder outright, resamples to the mix rate, writes mono
//	              into the emitter's input ring.
//	audio thread  reads that ring, and never learns a stream is involved.
//
// A full ring makes the worker sleep rather than drop, which is the opposite of what
// a procedural DSP source wants and the right call here: a decoder can always be
// asked again later, so falling behind costs latency, not samples.

#include "stAudioDecoder.h"
#include "stAudioEngine.h"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace st::audio
{
	class StreamPlayer
	{
	public:
		struct Config
		{
			bool loop = false;
			float volume = 1.0f;
			Submix submix = Submix::Music;
			// Spatial streams work, and cost what any 3D emitter costs. Music wants
			// false; a jukebox standing in the world wants true.
			bool spatial = false;
			std::string name = "stream";
		};

		StreamPlayer();
		~StreamPlayer();
		StreamPlayer(const StreamPlayer&) = delete;
		StreamPlayer& operator=(const StreamPlayer&) = delete;

		// Read the file, keep its bytes, create an emitter and start feeding it.
		// Returns false when the audio engine is not running, the file is missing, or
		// the format is not one the decoders handle - having logged which.
		bool Open(const std::string& filename, const Config& config = {});

		// Same, from bytes already in hand (an asset package, a download). The block
		// is TAKEN: it lives inside the player for as long as the stream does.
		bool OpenFromMemory(std::vector<uint8_t>&& bytes, const Config& config = {});

		// Same, over a decoder the caller built - a procedural container, a format the
		// game added itself.
		bool OpenDecoder(AudioDecoderPtr decoder, const Config& config = {});

		// Stop the worker, destroy the emitter, free the bytes. Idempotent, and called
		// by the destructor.
		void Close();

		bool IsOpen() const;
		// True once the decoder ran out AND the buffered tail has been played, at which
		// point the emitter is stopped too. Always false for a looping stream.
		bool IsFinished() const;

		void  SetVolume(float volume01);
		float GetVolume() const;
		void  SetLoop(bool loop);
		bool  GetLoop() const;
		void  SetPaused(bool paused);
		bool  IsPaused() const;

		// Play position in seconds. Accurate to about one mix block - the decoder runs
		// ahead of the speaker by whatever is sitting in the ring.
		double GetTimeSeconds() const;
		// Stream length, or a negative value when the container does not say.
		double GetLengthSeconds() const;
		// Ask the worker to seek. Returns immediately; the jump lands within a block.
		// Silently clamped to the stream, and a no-op on a decoder that cannot seek.
		void SeekSeconds(double seconds);

		// The emitter carrying the stream, or null when nothing is open.
		EmitterRef GetEmitter() const;
		// The decoder's own format, for a UI that wants to say what is playing.
		AudioFormat GetFormat() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

	using StreamRef = std::shared_ptr<StreamPlayer>;

	// Fire-and-forget: open a stream and hand it back. Holding the returned reference
	// is what keeps it playing - dropping it closes the stream.
	StreamRef PlayStream(const std::string& filename, const StreamPlayer::Config& config = {});
}
