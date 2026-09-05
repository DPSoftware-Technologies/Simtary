// stAudioLegacy: wi::audio implemented on top of st::audio
//
// This file REPLACES Engine/wiAudio.cpp. It provides the whole wi::audio public API
// (Sound / SoundInstance / 3D / submixes / reverb) backed by the OpenAL + Steam Audio
// engine instead of XAudio2, FAudio or miniaudio, so everything already written
// against the old API keeps working and starts going through the new one:
//
//	Scene::RunSoundUpdateSystem   SoundComponent on an entity - now spatialized by
//	                              Steam Audio rather than by X3DAudio
//	Scene lip sync                GetSampleInfo / GetTotalSamplesPlayed
//	wiAudio_BindLua               the Lua audio API
//	wi::resourcemanager           CreateSound for .wav / .ogg resources
//
// wiAudio.cpp is removed from the build in Engine/CMakeLists.txt. It is what crashed:
// wi::audio::Initialize() is deliberately never called, so its `audio_internal` stayed
// null and SetVolume dereferenced it the moment a scene contained a SoundComponent.
// Making those functions null-guarded no-ops would have stopped the crash and left
// every SoundComponent in the project silent; routing them here makes them work.
//
// What is intentionally different from XAudio2:
//	SetReverb    a no-op. Steam Audio's reverb is GEOMETRIC - it comes from the scene
//	             mesh and the materials on it, not from a preset - so there is nothing
//	             honest to map a "concert hall" enum onto. Emitters get real
//	             reflections by enabling them per emitter (see stAudioComponents.h).
//	SoundInstance::ENABLE_REVERB  likewise: it turns Steam Audio reflections on for
//	             that instance instead of routing it to a preset reverb submix.

#include "wiAudio.h"
#include "stAudioEngine.h"
#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiVector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace wi::audio
{
	namespace
	{
		// SUBMIX_TYPE is serialized, so its four values are fixed. The engine has five
		// groups; USER0/USER1 land on Voice/UI, which is what projects have used them
		// for, and nothing is lost because the volumes are independent either way.
		st::audio::Submix ToSubmix(SUBMIX_TYPE type)
		{
			switch (type)
			{
			case SUBMIX_TYPE_MUSIC: return st::audio::Submix::Music;
			case SUBMIX_TYPE_USER0: return st::audio::Submix::Voice;
			case SUBMIX_TYPE_USER1: return st::audio::Submix::UI;
			default:                return st::audio::Submix::SoundEffect;
			}
		}

		struct SoundInternal
		{
			st::audio::AudioClip clip;

			// wi::audio::SampleInfo hands out `const short*` and the scene's lip-sync
			// reads it directly, but clips are decoded to float. The int16 mirror is
			// built once, on the first GetSampleInfo, and only for sounds that actually
			// get asked - a sound nothing lip-syncs never pays for it.
			std::once_flag mirrorOnce;
			wi::vector<short> mirror;
		};

		struct SoundInstanceInternal
		{
			st::audio::EmitterRef emitter;
			st::audio::AudioClip clip;   // kept so GetTotalSamplesPlayed knows the rate
		};

		SoundInternal* to_internal(const Sound* param)
		{
			return param ? static_cast<SoundInternal*>(param->internal_state.get()) : nullptr;
		}
		SoundInstanceInternal* to_internal(const SoundInstance* param)
		{
			return param ? static_cast<SoundInstanceInternal*>(param->internal_state.get()) : nullptr;
		}

		// The listener the legacy 3D API drives. A scene with no stAudioCollector on it
		// still needs ears, and Update3D is the only place the old API says where they
		// are. Priority is the floor, so ANY real collector component outranks it and
		// this one stops being primary the moment the scene has one of its own.
		st::audio::CollectorRef g_legacyCollector;
		std::once_flag g_legacyCollectorOnce;

		st::audio::CollectorRef LegacyCollector()
		{
			std::call_once(g_legacyCollectorOnce, []
			{
				st::audio::AudioEngine& engine = st::audio::AudioEngine::Get();
				if (!engine.IsInitialized())
					return;
				g_legacyCollector = engine.CreateCollector("legacy-listener");
				g_legacyCollector->SetPriority(std::numeric_limits<int>::min());
			});
			return g_legacyCollector;
		}
	}

	void Initialize()
	{
		// Kept so anything still calling the old entry point brings the engine up
		// rather than silently doing nothing. InitializeOpenAL is the real one and is
		// what wi::initializer calls.
		InitializeOpenAL();
	}

	bool CreateSound(const std::string& filename, Sound* sound)
	{
		if (sound == nullptr)
			return false;
		st::audio::AudioClip clip = st::audio::LoadClip(filename);
		if (!clip)
			return false;
		auto internal_state = wi::allocator::make_shared<SoundInternal>();
		internal_state->clip = clip;
		sound->internal_state = internal_state;
		return true;
	}

	bool CreateSound(const uint8_t* data, size_t size, Sound* sound)
	{
		if (sound == nullptr)
			return false;
		st::audio::AudioClip clip = st::audio::LoadClipFromMemory(data, size);
		if (!clip)
			return false;
		auto internal_state = wi::allocator::make_shared<SoundInternal>();
		internal_state->clip = clip;
		sound->internal_state = internal_state;
		return true;
	}

	bool CreateSoundInstance(const Sound* sound, SoundInstance* instance)
	{
		if (instance == nullptr || sound == nullptr || !sound->IsValid())
			return false;
		st::audio::AudioEngine& engine = st::audio::AudioEngine::Get();
		if (!engine.IsInitialized())
			return false;

		SoundInternal* soundinternal = to_internal(sound);
		if (soundinternal == nullptr || !soundinternal->clip)
			return false;

		auto internal_state = wi::allocator::make_shared<SoundInstanceInternal>();
		internal_state->clip = soundinternal->clip;
		internal_state->emitter = engine.CreateEmitter("wi::audio");
		if (!internal_state->emitter)
			return false;

		st::audio::Emitter& emitter = *internal_state->emitter;
		emitter.SetClip(soundinternal->clip);
		emitter.SetSubmix(ToSubmix(instance->type));
		emitter.SetLoop(instance->IsLooped());
		emitter.SetPlayRegion(instance->begin, instance->length);
		// loop_begin/loop_length are documented as relative to the instance begin, so
		// they are rebased onto clip time before the emitter sees them.
		if (instance->loop_begin > 0.0f || instance->loop_length > 0.0f)
			emitter.SetLoopRegion(instance->begin + instance->loop_begin, instance->loop_length);

		// The old API has no 2D/3D switch on the instance itself - Update3D is what
		// makes a sound positional. Start non-spatial; the first Update3D promotes it.
		emitter.SetSpatial(false);

		if (instance->IsEnableReverb())
		{
			// The nearest honest reading of "reverb on" under a geometric spatializer:
			// let this source contribute to and receive Steam Audio reflections.
			emitter.SpatialSettings().reflections = true;
			emitter.ApplySpatialSettings();
		}

		instance->internal_state = internal_state;
		return true;
	}

	void Play(SoundInstance* instance)
	{
		// Called every frame by RunSoundUpdateSystem for anything marked playing, so it
		// has to be idempotent - Emitter::Play does not rewind, only Stop does.
		if (SoundInstanceInternal* internal_state = to_internal(instance))
			if (internal_state->emitter)
				internal_state->emitter->Play();
	}

	void Pause(SoundInstance* instance)
	{
		if (SoundInstanceInternal* internal_state = to_internal(instance))
			if (internal_state->emitter)
				internal_state->emitter->Pause();
	}

	void Stop(SoundInstance* instance)
	{
		if (SoundInstanceInternal* internal_state = to_internal(instance))
			if (internal_state->emitter)
				internal_state->emitter->Stop();
	}

	void SetVolume(float volume, SoundInstance* instance)
	{
		// SetVolume(v) with no instance is the MASTER volume - that overload is what
		// the default argument exists for. An instance that is merely INVALID is not
		// the same thing and must not fall through to master: RunSoundUpdateSystem
		// calls this every frame per sound, so one SoundComponent whose instance failed
		// to create would otherwise drag the master fader to its own volume forever.
		// (Reaching for the master here on a null backend is also exactly what crashed
		// wiAudio.cpp.)
		if (instance == nullptr)
		{
			st::audio::AudioEngine::Get().SetMasterVolume(volume);
			return;
		}
		SoundInstanceInternal* internal_state = to_internal(instance);
		if (internal_state == nullptr || !internal_state->emitter)
			return;
		internal_state->emitter->SetVolume(volume);
	}

	float GetVolume(const SoundInstance* instance)
	{
		if (instance == nullptr)
			return st::audio::AudioEngine::Get().GetMasterVolume();
		SoundInstanceInternal* internal_state = to_internal(instance);
		if (internal_state == nullptr || !internal_state->emitter)
			return 0.0f;
		return internal_state->emitter->GetVolume();
	}

	void ExitLoop(SoundInstance* instance)
	{
		if (SoundInstanceInternal* internal_state = to_internal(instance))
			if (internal_state->emitter)
				internal_state->emitter->SetLoop(false);
	}

	bool IsEnded(SoundInstance* instance)
	{
		SoundInstanceInternal* internal_state = to_internal(instance);
		if (internal_state == nullptr || !internal_state->emitter)
			return true;
		return !internal_state->emitter->IsPlaying() && !internal_state->emitter->IsPaused();
	}

	SampleInfo GetSampleInfo(const Sound* sound)
	{
		SampleInfo info;
		SoundInternal* internal_state = to_internal(sound);
		if (internal_state == nullptr || !internal_state->clip)
			return info;

		const st::audio::ClipData& clip = *internal_state->clip;
		std::call_once(internal_state->mirrorOnce, [internal_state, &clip]
		{
			// One extra sample: the scene's lip sync indexes with
			// std::min(current + n, sample_count) - inclusive of sample_count - so the
			// array has to be valid at that index.
			internal_state->mirror.resize(clip.samples.size() + 1);
			for (size_t i = 0; i < clip.samples.size(); ++i)
			{
				const float v = std::clamp(clip.samples[i], -1.0f, 1.0f);
				internal_state->mirror[i] = (short)(v * 32767.0f);
			}
			internal_state->mirror[clip.samples.size()] = 0;
		});

		info.samples = internal_state->mirror.data();
		info.sample_count = clip.samples.size();
		info.sample_rate = clip.sampleRate;
		info.channel_count = (uint32_t)clip.channels;
		return info;
	}

	uint64_t GetTotalSamplesPlayed(const SoundInstance* instance)
	{
		SoundInstanceInternal* internal_state = to_internal(instance);
		if (internal_state == nullptr || !internal_state->emitter || !internal_state->clip)
			return 0;
		// Per-CHANNEL frame count, which is what the lip-sync math multiplies back up
		// by channel_count.
		const double seconds = (double)internal_state->emitter->GetTime();
		return (uint64_t)std::max(0.0, seconds * (double)internal_state->clip->sampleRate);
	}

	void SetSubmixVolume(SUBMIX_TYPE type, float volume)
	{
		st::audio::AudioEngine::Get().SetSubmixVolume(ToSubmix(type), volume);
	}

	float GetSubmixVolume(SUBMIX_TYPE type)
	{
		return st::audio::AudioEngine::Get().GetSubmixVolume(ToSubmix(type));
	}

	void Update3D(SoundInstance* instance, const SoundInstance3D& instance3D)
	{
		SoundInstanceInternal* internal_state = to_internal(instance);
		if (internal_state == nullptr || !internal_state->emitter)
			return;

		// Being given a 3D pose is what makes this instance positional.
		st::audio::Emitter& emitter = *internal_state->emitter;
		if (!emitter.IsSpatial())
			emitter.SetSpatial(true);

		st::audio::SpatialTransform emitterTransform;
		emitterTransform.position = instance3D.emitterPos;
		emitterTransform.forward = instance3D.emitterFront;
		emitterTransform.up = instance3D.emitterUp;
		emitterTransform.velocity = instance3D.emitterVelocity;
		emitter.SetTransform(emitterTransform);

		if (instance3D.emitterRadius > 0.0f)
		{
			// The old emitterRadius is the size of the sound source, which is exactly
			// what Steam Audio's volumetric occlusion radius means.
			st::audio::EmitterSpatialSettings& settings = emitter.SpatialSettings();
			settings.occlusionRadius = instance3D.emitterRadius;
			emitter.ApplySpatialSettings();
		}

		if (st::audio::CollectorRef collector = LegacyCollector())
		{
			st::audio::SpatialTransform listener;
			listener.position = instance3D.listenerPos;
			// SoundInstance3D calls the listener's forward vector "At", matching the
			// camera it is copied from.
			listener.forward = instance3D.listenerFront;
			listener.up = instance3D.listenerUp;
			listener.velocity = instance3D.listenerVelocity;
			collector->SetTransform(listener);
		}
	}

	void SetReverb(REVERB_PRESET preset)
	{
		// Deliberately inert - see the header comment. Logged once so a project that
		// depends on presets finds out rather than wondering why nothing changed.
		static std::once_flag once;
		std::call_once(once, []
		{
			wilog("wi::audio::SetReverb: ignored. Steam Audio derives reverb from scene "
				"geometry and materials, not from presets - enable reflections per emitter instead.");
		});
		(void)preset;
	}
}
