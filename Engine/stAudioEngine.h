#pragma once
// stAudioEngine: the audio system
//
// Two backends, one job each, and the split is the whole design:
//
//	OpenAL Soft   device I/O and 2D. It owns the output device, the context, and a
//	              pool of static voices for UI clicks, music and anything else that
//	              is not in the world. Those voices are AL_SOURCE_RELATIVE with the
//	              position pinned at the listener, so OpenAL mixes them and never
//	              spatializes them.
//	Steam Audio   everything 3D. Distance, air, directivity, occlusion, transmission,
//	              reflections, pathing, HRTF. Its output is ONE stereo bus, streamed
//	              into ONE extra OpenAL source that is likewise pinned and unpanned.
//
// Nothing is spatialized twice, and neither library is asked to do the thing it is
// worse at.
//
// the object model
//
//	Emitter    a speaker. Has a position, a sound (a clip, or samples you push), and
//	           the full set of Steam Audio source options. Its Input() buffer takes
//	           procedural audio; its Output() buffer is a tap of what it actually
//	           emitted, post-gain and pre-spatialization.
//	Collector  a microphone. Has a position and the Steam Audio listener options, and
//	           renders every audible emitter from ITS OWN point of view into its
//	           Output() buffer. A scene can hold many: the one with the highest
//	           priority is what the player hears, and the rest are still rendering -
//	           a tape recorder in a room, a radio mic on an NPC, a hydrophone.
//
// Both are plain engine objects. AudioEmitterComponent / AudioCollectorComponent in
// stAudioComponents.h bolt them onto scene entities and drive the transforms; game
// code that is not entity-shaped can create them directly.
//
// threading
//
//	game thread   creates and destroys emitters/collectors, sets parameters, pushes
//	              procedural samples, reads Output() taps. Never blocks on audio.
//	audio thread  pulls one block per emitter, runs the Steam Audio chain per
//	              collector, and hands the primary collector's stereo to OpenAL.
//	              Never allocates in steady state and never takes a game-thread lock
//	              for longer than a pointer swap.
//	sim thread    re-runs Steam Audio's reflection and pathing ray tracing at
//	              SimulationSettings::updateRateHz.

#include "stAudioBuffer.h"
#include "stAudioClip.h"
#include "stAudioSpatial.h"
#include "CommonInclude.h"
#include "wiMath.h"   // XMFLOAT3 (see the note in stAudioSpatial.h)

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace st::audio
{
	// Defined in stAudioStream.h. Declared here so the engine can hand back the
	// player behind streamed music without every consumer of this header pulling in
	// the decoder API.
	class StreamPlayer;

	// mixer routing
	// A submix is a volume group. Independent of 2D/3D: a footstep is a SoundEffect
	// whether it is spatialized or not.
	enum class Submix : uint32_t
	{
		SoundEffect,
		Music,
		Voice,
		UI,
		Ambient,
		Count,
	};

	struct EngineConfig
	{
		int sampleRate = 48000;
		// One spatial block. 512 frames at 48 kHz is ~10.7 ms: short enough that head
		// rotation does not smear the HRTF, long enough that the ray tracer and the
		// convolution have room. Steam Audio's effects run at exactly this size.
		int frameSize = 512;
		// Queued blocks on the streaming source. Three gives ~32 ms of slack against
		// a scheduler hiccup without adding audible latency to head movement.
		int queuedBlocks = 3;
		int maxVoices2D = 64;         // OpenAL static sources for the 2D pool
		bool floatOutput = true;      // use AL_EXT_FLOAT32 when the driver has it
		SimulationSettings simulation;
	};

	// Emitter
	class Emitter
	{
	public:
		~Emitter();

		// source
		// A clip and a pushed buffer are mutually exclusive: setting a clip discards
		// whatever is in Input(), and the first Write() to Input() takes over from the
		// clip. An emitter is either playing an asset or being fed.
		void SetClip(const AudioClip& clip);
		AudioClip GetClip() const;

		// Procedural / streamed input. Mono for anything spatialized - a stereo push is
		// downmixed, with the stereo image discarded, because a point in space has one
		// signal. Push at least one block ahead of the audio thread or it reads silence.
		AudioBuffer& Input();

		// Tap of what this emitter actually emitted this block: post-volume, post-pitch,
		// PRE-spatialization, mono. Read it for a VU meter, a lip-sync envelope, a
		// "is the engine loud enough to mask footsteps" gameplay query, or to record
		// the dry signal. Never consumed by the engine, so reading is free.
		const AudioBuffer& Output() const;
		AudioBuffer& Output();

		// transport
		void Play();
		void PlayDelayed(float seconds);
		void Pause();
		void Stop();
		bool IsPlaying() const;
		bool IsPaused() const;
		// Playback position in seconds. Setting it seeks; only meaningful with a clip.
		float GetTime() const;
		void  SetTime(float seconds);

		// Play only part of the clip. `lengthSeconds` of 0 means "to the end".
		// Playback starts at the region begin and stops (or loops) at its end.
		void SetPlayRegion(float beginSeconds, float lengthSeconds = 0.0f);
		// Where a looping emitter jumps back to. Relative to the START OF THE CLIP, not
		// to the play region. Zero length means "loop the whole play region", which is
		// the default and the common case.
		void SetLoopRegion(float beginSeconds, float lengthSeconds = 0.0f);

		// mixing
		void  SetVolume(float volume01);
		float GetVolume() const;
		// 1 = unmodified. Resamples the clip on the fly, so it shifts pitch AND speed,
		// exactly like Unity's AudioSource.pitch.
		void  SetPitch(float pitch);
		float GetPitch() const;
		void  SetLoop(bool loop);
		bool  GetLoop() const;
		void  SetSubmix(Submix submix);
		Submix GetSubmix() const;
		// false routes this emitter to the 2D path (OpenAL, no spatialization at all)
		// instead of through Steam Audio. The Unity spatialBlend == 0 case, but as a
		// routing decision rather than a blend, because there is no reason to pay for a
		// ray-traced chain and then throw the result away.
		void SetSpatial(bool spatial);
		bool IsSpatial() const;
		// Destroy the emitter automatically when a non-looping clip finishes. What
		// PlayClipAtPoint uses; off by default for emitters you own.
		void SetAutoDestroy(bool value);

		// 3D
		// Every Steam Audio source option. Assign the struct, or poke one field and
		// call ApplySpatialSettings(); either way it takes effect on the next frame.
		EmitterSpatialSettings& SpatialSettings();
		const EmitterSpatialSettings& SpatialSettings() const;
		void ApplySpatialSettings();

		// Render-origin-relative position/orientation/velocity, pushed every frame by
		// the component (or by hand for a component-less emitter).
		void SetTransform(const SpatialTransform& transform);
		void SetPosition(const XMFLOAT3& position);
		const SpatialTransform& GetTransform() const;

		// What the simulator concluded about this emitter last pass: distance,
		// attenuation, occlusion, transmission. Gameplay-readable - this is how "can
		// the guard hear the player" gets an answer that agrees with what the player
		// hears.
		SpatialResult GetSpatialResult() const;

		// identity
		void SetName(const std::string& name);
		const std::string& GetName() const;

	private:
		Emitter();
		friend class AudioEngine;
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
	using EmitterRef = std::shared_ptr<Emitter>;

	// Collector
	class Collector
	{
	public:
		~Collector();

		// The collector's own rendering of the whole scene, filled every block:
		// ChannelCountFor(layout) channels for Binaural/Panning output - 1, 2, 5 or 7 -
		// and (order+1)^2 channels for Ambisonics. A Tap buffer, so reading never steals
		// from another reader - a recorder and a level meter can watch the same
		// microphone.
		//
		// The channel count is FIXED at CreateCollector: a game-thread reader holds
		// pointers into it, so the audio thread cannot reallocate underneath. Changing
		// the layout of a live collector means destroying it and creating another, which
		// is what stAudioCollector does when the field changes.
		const AudioBuffer& Output() const;
		AudioBuffer& Output();

		CollectorSpatialSettings& SpatialSettings();
		const CollectorSpatialSettings& SpatialSettings() const;
		void ApplySpatialSettings();

		void SetTransform(const SpatialTransform& transform);
		const SpatialTransform& GetTransform() const;

		void  SetVolume(float volume01);
		float GetVolume() const;

		// Highest priority among the enabled collectors becomes the primary: its render
		// is what goes to the speakers, and its position is what the ray tracer
		// simulates from. Ties break towards whichever was created first.
		void SetPriority(int priority);
		int  GetPriority() const;
		bool IsPrimary() const;

		// A collector that is not routed to the device still renders into Output().
		// That is the point of it: a microphone recording a room the player is not in.
		// Turning this off on the primary mutes the game.
		void SetRouteToOutput(bool value);
		bool GetRouteToOutput() const;

		// Stop rendering entirely - a microphone that is switched off costs nothing.
		void SetEnabled(bool value);
		bool IsEnabled() const;

		void SetName(const std::string& name);
		const std::string& GetName() const;

	private:
		Collector();
		friend class AudioEngine;
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
	using CollectorRef = std::shared_ptr<Collector>;

	// engine
	class AudioEngine
	{
	public:
		static AudioEngine& Get();

		bool Initialize(const EngineConfig& config = {});
		void Shutdown();
		bool IsInitialized() const;

		const EngineConfig& GetConfig() const;
		int GetSampleRate() const;
		int GetFrameSize() const;
		const char* GetDeviceName() const;
		// True when Steam Audio is really behind the 3D path (see stAudioSpatial.h).
		bool IsSpatialAvailable() const;

		// Called once per frame from the main loop, before or after the scene update.
		// Publishes the primary collector, retires finished auto-destroy emitters, and
		// paces the reflection/pathing job. Does no DSP.
		void Update(float dt);

		// factories
		// The engine keeps a reference of its own, so an emitter stays alive and
		// audible after the caller drops the handle - until Destroy() or auto-destroy.
		EmitterRef CreateEmitter(const std::string& name = "emitter");
		// The collector's output buffer is allocated with a fixed channel count, so the
		// settings that decide that count - layout, output mode, ambisonic order - have
		// to be handed in here rather than pushed in afterwards.
		CollectorRef CreateCollector(const std::string& name = "collector",
			const CollectorSpatialSettings& settings = {});
		void Destroy(const EmitterRef& emitter);
		void Destroy(const CollectorRef& collector);

		CollectorRef GetPrimaryCollector() const;
		void GetEmitters(std::vector<EmitterRef>& out) const;
		void GetCollectors(std::vector<CollectorRef>& out) const;

		// volumes
		void  SetMasterVolume(float volume01);
		float GetMasterVolume() const;
		void  SetSubmixVolume(Submix submix, float volume01);
		float GetSubmixVolume(Submix submix) const;

		// Global pause: stops the device without tearing down any state.
		void SetPaused(bool paused);
		bool IsPaused() const;

		// diagnostics
		struct Stats
		{
			int activeEmitters = 0;       // emitters that produced signal last block
			int spatialEmitters = 0;      // of those, how many went through Steam Audio
			int activeCollectors = 0;
			int voices2D = 0;             // OpenAL static voices in use
			uint64_t blocksRendered = 0;
			uint64_t underruns = 0;       // blocks the device wanted and did not get
			float mixLoad = 0.0f;         // fraction of a block period spent mixing
			float peakOutput = 0.0f;      // last block's peak, for a master meter
		};
		Stats GetStats() const;

	private:
		AudioEngine();
		~AudioEngine();
		AudioEngine(const AudioEngine&) = delete;
		AudioEngine& operator=(const AudioEngine&) = delete;
		struct Impl;
		std::unique_ptr<Impl> impl_;

		// The free helpers below are the engine's own public face; they reach into the
		// 2D voice pool and the music slot, which nothing outside this file should.
		friend void PlayOneShot(const AudioClip&, float, Submix, float);
		friend EmitterRef PlayMusic(const std::string&, float, bool, bool);
		friend void StopMusic();
		friend std::shared_ptr<StreamPlayer> GetMusicStream();
		friend void StopAll();
	};

	// the easy API
	// Unity's static AudioSource helpers, for the ninety per cent of calls that do not
	// want an object: fire and forget.

	// Engine mix rate. Clip loading resamples to it; safe to call before Initialize()
	// (it answers with the configured default).
	int GetMixSampleRate();
	int GetMixFrameSize();

	// 2D one-shot straight through OpenAL - a UI click, a notification. No
	// spatialization, no Steam Audio, no emitter object.
	void PlayOneShot(const AudioClip& clip, float volume = 1.0f, Submix submix = Submix::UI, float pitch = 1.0f);
	void PlayOneShot(const std::string& filename, float volume = 1.0f, Submix submix = Submix::UI, float pitch = 1.0f);

	// 3D one-shot at a world point - Unity's AudioSource.PlayClipAtPoint. Creates a
	// throwaway emitter that destroys itself when the clip ends.
	EmitterRef PlayClipAtPoint(const AudioClip& clip, const XMFLOAT3& position, float volume = 1.0f,
		Submix submix = Submix::SoundEffect);
	EmitterRef PlayClipAtPoint(const std::string& filename, const XMFLOAT3& position, float volume = 1.0f,
		Submix submix = Submix::SoundEffect);

	// Looping 2D music on its own submix. Returns the emitter so it can be stopped or
	// faded; passing an empty filename stops whatever is playing.
	//
	// `stream` is the default because a track is the one asset a game should never
	// decode whole: streaming keeps the compressed bytes and decodes a block at a time
	// (see stAudioStream.h), which is megabytes instead of the hundred-odd a
	// five-minute track costs as float. Pass false to force the old full-decode path -
	// worth it only for a short sting that is restarted constantly, where a cached
	// clip beats spinning up a worker thread. A stream that fails to open falls back
	// to a clip on its own, so a format the streamer chokes on still plays.
	//
	// Reach the stream itself (seek, position, length) with GetMusicStream().
	EmitterRef PlayMusic(const std::string& filename, float volume = 1.0f, bool loop = true,
		bool stream = true);
	void StopMusic();

	// The StreamPlayer behind the current music, or null when music is not playing or
	// was loaded as a clip. Include stAudioStream.h to do anything with it - seek,
	// read the position, ask how long the track is.
	std::shared_ptr<StreamPlayer> GetMusicStream();

	// Stop every emitter and every 2D voice. Scene transitions.
	void StopAll();
}
