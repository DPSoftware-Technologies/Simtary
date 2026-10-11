#pragma once
// AudioPreview - listen to a sound asset from the Resource Explorer.
//
// Plays through the engine's own audio path (st::audio: an Emitter on the 2D route, UI
// submix), so what you hear is what the game would play - WAV, Ogg Vorbis, QOA and Opus,
// read through the mounted asset package like any other asset. Not spatialized: a
// preview has no position.
//
//	inspector   play / pause / stop, loop, volume, time, and the waveform - click or drag
//	            on it to seek
//	tiles       a play button on sound tiles, Unity-style; clicking it toggles playback
//
// One sound at a time: previewing another asset stops the last one.

#include <cstdint>
#include <string>
#include <vector>

#include "stAudioClip.h"
#include "stAudioEngine.h"

namespace st::devui {

class AudioPreview {
public:
    ~AudioPreview ();

    // Point the preview at an asset. `loadName` resolves through the asset source (a
    //	package name or a file on disk); `bytes`, when given, is used instead (a working-set
    //	row that exists only in memory). Switching assets stops playback.
    void Select (uint64_t id, const std::string& loadName, const std::vector<uint8_t>* bytes = nullptr,
                 uint64_t sizeHint = 0);
    // The inspector block for the selected asset.
    void DrawInspector ();

    bool IsPlaying (uint64_t id) const;
    // Tile button: play `id` from the start, or stop it if it is the one playing.
    void Toggle (uint64_t id, const std::string& loadName, const std::vector<uint8_t>* bytes = nullptr);
    void Stop ();

private:
    bool EnsureClip ();
    void EnsureEmitter ();
    void Play ();
    void BuildPeaks (int columns);

    uint64_t id_ = 0;
    std::string name_;
    std::vector<uint8_t> bytes_;
    bool fromMemory_ = false;
    uint64_t sizeHint_ = 0;

    st::audio::AudioClip clip_;
    bool loadFailed_ = false;
    st::audio::EmitterRef emitter_;

    bool  loop_ = false;
    float volume_ = 0.8f;
    float startAt_ = 0.0f;   // where Play starts from when stopped (set by seeking)

    std::vector<float> peakMin_, peakMax_;
};

} // namespace st::devui
