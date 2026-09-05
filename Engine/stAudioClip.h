#pragma once
// stAudioClip: decoded sample data, Unity's AudioClip
//
// A clip is immutable, refcounted and shared: loading "assets/contents/shot.wav"
// twice hands back the same decoded block. Handing a clip to ten emitters costs ten
// pointers, not ten copies.
//
// Everything is decoded up front to interleaved float at the ENGINE's mix rate, so
// the mixer never resamples and never converts formats on the audio thread. That
// trades memory for a mixer that cannot stall: a minute of 48 kHz stereo float is
// ~23 MB, which is the wrong deal for music - stream those through an Emitter's
// AudioBuffer instead of loading them as a clip.
//
// Formats: RIFF/WAVE (PCM 8/16/24/32-bit integer and 32-bit float) and Ogg Vorbis.

#include "stAudioBuffer.h"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace st::audio
{
	struct ClipData
	{
		std::vector<float> samples;   // interleaved, already at the engine mix rate
		int channels = 0;
		int sampleRate = 0;
		std::string name;

		int GetFrameCount() const { return channels > 0 ? (int)(samples.size() / (size_t)channels) : 0; }
		float GetLengthSeconds() const { return sampleRate > 0 ? (float)GetFrameCount() / (float)sampleRate : 0.0f; }
	};

	// Handle type. Copy it freely; the data dies with the last handle (and the cache
	// entry, which holds only a weak reference).
	using AudioClip = std::shared_ptr<const ClipData>;

	// Load and decode, resampling to `targetSampleRate` (0 = the engine's current mix
	// rate). Returns null on a missing file or an unsupported format, having logged why.
	// Cached by filename+rate: the second call is a map lookup.
	AudioClip LoadClip(const std::string& filename, int targetSampleRate = 0);

	// Same, from memory already in hand (an archive, a network payload). `name` is
	// only used for logging and is NOT a cache key - in-memory loads never cache.
	AudioClip LoadClipFromMemory(const uint8_t* data, size_t size, const std::string& name = "memory",
		int targetSampleRate = 0);

	// Build a clip from samples the game generated itself. Interleaved input, taken as
	// already being at `sampleRate` and resampled if that is not the mix rate.
	AudioClip CreateClip(const float* interleaved, int frames, int channels, int sampleRate,
		const std::string& name = "procedural", int targetSampleRate = 0);

	// Drop cache entries no clip handle is keeping alive. Called on scene unload; safe
	// any time, since live clips are held by their handles rather than by the cache.
	void TrimClipCache();
	// Drop every cache entry, live or not (the data itself survives in live handles).
	void ClearClipCache();
}
