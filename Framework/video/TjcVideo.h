#pragma once
// st::video::TjcVideo - a TJC stream (Framework/video/tjc.h) played on the CPU.
//
// TJC is a tiled-JPEG codec with motion compensation and an interleaved QOA audio
// track. Decoding is integer-only and cheap, which is why it suits the startup splash:
// it runs before the world is ready, on every platform, with no GPU video decoder.
//
// This class owns the compressed bytes and one decoder, and keeps an RGBA8 picture
// of the current frame. Only the tiles a frame actually carried are converted from
// YUV, so a mostly still logo costs almost nothing per frame. Nothing here touches
// the GPU - the splash (render/detroit/DetroitSplash) uploads Pixels() itself.
//
// The audio track is pulled out once, at Open, by a parse-only pass over the stream
// (no picture is decoded for it), and handed out as an st::audio decoder so the
// engine's StreamPlayer can play it. The engine's own QOA reader cannot read it: a
// .qoa file has fixed 5120-sample frames and it seeks by arithmetic, while TJC cuts
// the track into one short QOA frame per video frame.
//
// Main thread only.

#include "stAudioDecoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tjc { class Decoder; }

namespace st::video {

// Which YUV -> RGB matrix the stream was encoded with. TJC does not store it; an
// encode from an HD source almost always keeps BT.709, an SD one BT.601.
enum class ColorMatrix {
    Auto,    // BT.709 from 720 lines up, BT.601 below
    BT601,
    BT709,
};

class TjcVideo {
public:
    TjcVideo();
    ~TjcVideo();
    TjcVideo(const TjcVideo&) = delete;
    TjcVideo& operator=(const TjcVideo&) = delete;

    // Read the file through wi::helper::FileRead, so it resolves out of a mounted
    // asset package like any other asset.
    bool Open(const std::string& path, std::string* error = nullptr);
    // Same, over bytes already in hand. The block is TAKEN.
    bool OpenFromMemory(std::vector<uint8_t>&& bytes, std::string* error = nullptr);
    void Close();
    bool IsOpen() const { return decoder_ != nullptr; }

    void SetColorMatrix(ColorMatrix matrix) { matrix_ = matrix; }

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    uint32_t FrameCount() const { return frameCount_; }
    double FrameRate() const { return frameRate_; }
    double Duration() const { return frameRate_ > 0 ? frameCount_ / frameRate_ : 0.0; }

    // Decode forward to the frame that is on screen at `seconds` from the start.
    // Frames cannot be skipped (each one patches the previous picture), so a caller
    // that fell behind catches up at most `maxDecodes` frames per call. Returns true
    // when Pixels() changed.
    bool AdvanceTo(double seconds, int maxDecodes = 4);
    // The last frame has been decoded, or the stream ended early / was damaged.
    bool Finished() const { return finished_; }

    // The current picture, Width() x Height() RGBA8 (R in the lowest byte), opaque.
    // Black until the first AdvanceTo.
    const uint32_t* Pixels() const { return pixels_.data(); }

    bool HasAudio() const { return audioSamples_ > 0; }
    // A decoder over a copy of the audio track, for st::audio::StreamPlayer::OpenDecoder.
    // Null when the stream has no audio.
    st::audio::AudioDecoderPtr CreateAudioDecoder() const;

private:
    bool Fail(std::string* error, const std::string& message);
    void ConvertTiles();

    std::vector<uint8_t> bytes_;
    size_t readPos_ = 0;
    tjc::Decoder* decoder_ = nullptr;

    uint32_t width_ = 0, height_ = 0;
    uint32_t frameCount_ = 0;
    double frameRate_ = 0.0;
    int decodedFrame_ = -1;
    bool finished_ = false;
    ColorMatrix matrix_ = ColorMatrix::Auto;

    std::vector<uint32_t> pixels_;
    std::vector<uint8_t> dirty_;      // per tile: needs converting into pixels_
    std::vector<int> dirtyList_;

    // Every frame's QOA frames, back to back, without a .qoa file header.
    std::vector<uint8_t> audio_;
    int audioChannels_ = 0;
    uint32_t audioRate_ = 0;
    int64_t audioSamples_ = 0;   // per channel
};

} // namespace st::video
