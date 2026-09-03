#pragma once
// ─── Audio Mixer: the DevUI window for the audio system ─────────────────────────
//
// One panel over both halves of the engine's audio stack, because they are two
// libraries doing two jobs on one signal and a problem in either shows up as
// "the sound is wrong":
//
//	OpenAL Soft   the output device and the 2D mixer. Which device opened, at what
//	              rate, whether float output was available, how many static voices
//	              the one-shot pool is using, how many blocks underran.
//	Steam Audio   everything 3D. Whether it is really loaded or the fallback panner
//	              is standing in, which ray tracer backend, the simulation budget.
//
// Plus the mixer proper: master and per-submix faders, a peak meter on the output,
// and a live row per Emitter and per Collector with its own level, so "is this
// emitter audible, and if not which stage killed it" is answerable by looking
// rather than by guessing. Emitter rows show the simulator's verdict - distance,
// attenuation, occlusion - which is the number that usually explains a silent sound.

namespace st::devui {

// Draw the mixer into the CURRENT window; no Begin/End. Use this to dock it into a
// panel of your own.
void AudioMixerGUI();

// Convenience: the standalone "Audio Mixer" window. `p_open` may be null.
void AudioMixerWindow(bool* p_open = nullptr);

}
