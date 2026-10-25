#pragma once
// st::render::detroit::StartupSplash - the in-engine splash over the first frames.
//
// The SDL splash in st::Run covers the window until the graphics device exists, and
// the native loading window covers the blocking startup scene load. Neither covers
// what comes after: the world is loaded, but the engine is still building object
// pipelines on background threads, and RenderMeshes SKIPS every draw whose pipeline is
// not ready yet - so the first seconds showed sky and nothing else, then objects
// popping in. This overlay sits on top of the composed frame until the world is worth
// looking at.
//
// Phases:
//   Video  the project's .tjc intro plays (skippable), with its audio
//   Hold   the last video frame, or the splash image, or just the background, until
//          st::App reports the world ready (or the wait times out)
//   Fade   the overlay fades out over the live game
//
// The game runs normally underneath the whole time - the scene updates and renders,
// which is exactly what makes the pipelines get used and the first uploads happen
// while nobody is looking. Input is held back by st::Run while BlocksInput().
//
// Detroit only: it draws with wi::image straight onto the swapchain pass.

#include "video/TjcVideo.h"
#include "stAudioStream.h"
#include "wiGraphicsDevice.h"
#include "wiCanvas.h"

#include <memory>
#include <string>
#include <vector>

namespace st::render::detroit {

struct SplashSettings {
    std::string image;                  // BMP shown when there is no video (and held after it if no frame)
    std::string video;                  // .tjc; empty = no video
    XMFLOAT3 background = XMFLOAT3(0, 0, 0);
    bool skippable = true;              // any key / click / tap ends the VIDEO, never the wait
    float minSeconds = 0.0f;            // shortest time on screen, video included
    float maxWaitSeconds = 30.0f;       // longest Hold before the overlay gives up and fades
    float fadeSeconds = 0.5f;
    float volume = 1.0f;
};

class StartupSplash {
public:
    enum class Phase { Off, Video, Hold, Fade };

    ~StartupSplash() { Shutdown(); }

    // Opens the video (a parse-only pass over it) and the image. Call once, after the
    // startup scene loaded; the video's clock starts on the first Update.
    void Start(const SplashSettings& settings);

    // Per frame on the main thread, with the real delta. `worldReady` is st::App's
    // verdict; `status` is shown under the picture while Hold waits on it.
    void Update(float dt, bool worldReady, const std::string& status);

    // Copies a new video frame to the GPU on its own command list. From
    // st::App::Render, i.e. before the frame's compose list is opened.
    void Upload();

    // Over everything already composed. Inside the swapchain render pass.
    void Draw(const wi::Canvas& canvas, wi::graphics::ColorSpace colorspace, wi::graphics::CommandList cmd);

    // End the video now (the Hold still waits for the world).
    void SkipVideo();

    bool IsActive() const { return phase_ != Phase::Off; }
    bool BlocksInput() const { return phase_ == Phase::Video || phase_ == Phase::Hold; }
    bool Skippable() const { return settings_.skippable && phase_ == Phase::Video; }
    Phase CurrentPhase() const { return phase_; }

    // Frees the decoder, the audio stream and the GPU textures. Called when the fade
    // ends, and by st::App::Exit before the device goes away.
    void Shutdown();

private:
    void EnterHold();
    bool LoadImage(const std::string& path);
    bool EnsureVideoTextures();

    SplashSettings settings_;
    Phase phase_ = Phase::Off;
    float elapsed_ = 0.0f;       // since Start
    float holdElapsed_ = 0.0f;   // since Hold began
    float fade_ = 1.0f;          // overlay opacity
    std::string status_;

    st::video::TjcVideo video_;
    double videoClock_ = 0.0;    // wall clock; the audio clock wins while audio plays
    bool videoFrameReady_ = false;   // video_ has a picture worth holding
    bool videoDirty_ = false;        // a new frame waits for Upload()
    std::unique_ptr<st::audio::StreamPlayer> audio_;

    wi::graphics::Texture videoTexture_;
    std::vector<wi::graphics::Texture> videoStaging_;   // one per frame in flight, + 1
    uint32_t stagingNext_ = 0;

    wi::graphics::Texture imageTexture_;
    uint32_t imageWidth_ = 0, imageHeight_ = 0;
};

} // namespace st::render::detroit
