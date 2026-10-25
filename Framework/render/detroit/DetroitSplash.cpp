#include "render/detroit/DetroitSplash.h"

#include "wiBacklog.h"
#include "wiFont.h"
#include "wiHelper.h"
#include "wiImage.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

using namespace wi::graphics;

namespace st::render::detroit {

void StartupSplash::Start(const SplashSettings& settings) {
    Shutdown();
    settings_ = settings;
    elapsed_ = 0.0f;
    holdElapsed_ = 0.0f;
    fade_ = 1.0f;
    videoClock_ = 0.0;

    // Held after the video when the video produced no picture, and the whole show when
    // there is no video. Failing to load it just leaves the background colour.
    if (!settings_.image.empty()) LoadImage(settings_.image);

    if (!settings_.video.empty()) {
        std::string error;
        if (video_.Open(settings_.video, &error)) {
            phase_ = Phase::Video;
            wi::backlog::post("Splash: " + settings_.video + ", " + std::to_string(video_.Width()) + "x" +
                              std::to_string(video_.Height()) + ", " + std::to_string(video_.FrameCount()) +
                              " frames" + (video_.HasAudio() ? ", with audio" : ""));
            if (video_.HasAudio()) {
                st::audio::StreamConfig config;
                config.volume = settings_.volume;
                config.submix = st::audio::Submix::Music;
                config.name = "splash";
                audio_ = std::make_unique<st::audio::StreamPlayer>();
                // No audio engine (no device, a server) is not a reason to drop the
                // picture; the wall clock times the video instead.
                if (!audio_->OpenDecoder(video_.CreateAudioDecoder(), config)) audio_.reset();
            }
            return;
        }
        wi::backlog::post("Splash video skipped: " + error, wi::backlog::LogLevel::Warning);
    }
    EnterHold();
}

void StartupSplash::EnterHold() {
    phase_ = Phase::Hold;
    holdElapsed_ = 0.0f;
}

void StartupSplash::SkipVideo() {
    if (phase_ != Phase::Video) return;
    audio_.reset();
    EnterHold();
}

void StartupSplash::Update(float dt, bool worldReady, const std::string& status) {
    if (phase_ == Phase::Off) return;

    // The first frame after the blocking startup arrives with a long delta. A video
    // timed by the wall clock would jump straight to its middle.
    dt = std::clamp(dt, 0.0f, 0.1f);
    elapsed_ += dt;
    status_ = status;

    switch (phase_) {
        case Phase::Video: {
            videoClock_ += dt;
            double t = videoClock_;
            // The audio clock is the one the ear hears, so the picture follows it - but
            // never more than a quarter second behind the wall clock, so a stalled audio
            // device cannot freeze the video.
            if (audio_ && audio_->IsOpen() && !audio_->IsFinished())
                t = std::max(audio_->GetTimeSeconds(), videoClock_ - 0.25);
            if (video_.AdvanceTo(t)) videoDirty_ = true;
            if (video_.Finished() && t >= video_.Duration()) EnterHold();
            break;
        }
        case Phase::Hold:
            holdElapsed_ += dt;
            if (worldReady && elapsed_ >= settings_.minSeconds) {
                wi::backlog::post("Splash: world ready, " + std::to_string(elapsed_) + " s on screen (" +
                                  std::to_string(holdElapsed_) + " s waiting after the video)");
                phase_ = Phase::Fade;
            } else if (holdElapsed_ >= settings_.maxWaitSeconds) {
                wi::backlog::post("Splash: gave up waiting for the world after " +
                                  std::to_string(int(settings_.maxWaitSeconds)) + " s (" + status_ + ")",
                                  wi::backlog::LogLevel::Warning);
                phase_ = Phase::Fade;
            }
            break;
        case Phase::Fade:
            fade_ -= dt / std::max(0.01f, settings_.fadeSeconds);
            if (audio_) audio_->SetVolume(settings_.volume * std::max(0.0f, fade_));
            if (fade_ <= 0.0f) Shutdown();
            break;
        case Phase::Off:
            break;
    }
}

bool StartupSplash::EnsureVideoTextures() {
    if (videoTexture_.IsValid()) return true;
    GraphicsDevice* device = GetDevice();

    TextureDesc td;
    td.width = video_.Width();
    td.height = video_.Height();
    td.format = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::SHADER_RESOURCE;
    td.usage = Usage::DEFAULT;
    td.layout = ResourceState::SHADER_RESOURCE;
    if (!device->CreateTexture(&td, nullptr, &videoTexture_)) return false;
    device->SetName(&videoTexture_, "st::StartupSplash::video");

    // One staging texture per frame the GPU may still be copying from, plus the one
    // being written: a frame's copy never reads a texture the CPU is filling.
    TextureDesc sd = td;
    sd.bind_flags = BindFlag::NONE;
    sd.usage = Usage::UPLOAD;
    sd.layout = ResourceState::COPY_SRC;
    videoStaging_.resize(GraphicsDevice::GetBufferCount() + 1);
    for (Texture& staging : videoStaging_) {
        if (!device->CreateTexture(&sd, nullptr, &staging)) {
            videoTexture_ = {};
            videoStaging_.clear();
            return false;
        }
        device->SetName(&staging, "st::StartupSplash::staging");
    }
    stagingNext_ = 0;
    return true;
}

void StartupSplash::Upload() {
    if (!videoDirty_ || !video_.IsOpen()) return;
    videoDirty_ = false;
    if (!EnsureVideoTextures()) {
        wi::backlog::post("Splash: cannot create the video texture", wi::backlog::LogLevel::Error);
        video_.Close();
        audio_.reset();
        EnterHold();
        return;
    }

    Texture& staging = videoStaging_[stagingNext_];
    stagingNext_ = (stagingNext_ + 1) % uint32_t(videoStaging_.size());
    if (staging.mapped_subresources == nullptr || staging.mapped_subresource_count == 0) return;
    const SubresourceData& sub = staging.mapped_subresources[0];
    uint8_t* dst = (uint8_t*)sub.data_ptr;
    if (dst == nullptr) return;

    // row_pitch is the driver's and can exceed width * 4, so rows go one at a time.
    const uint32_t w = video_.Width(), h = video_.Height();
    const uint32_t* src = video_.Pixels();
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(dst + size_t(y) * sub.row_pitch, src + size_t(y) * w, size_t(w) * 4);

    GraphicsDevice* device = GetDevice();
    CommandList cmd = device->BeginCommandList();
    const GPUBarrier toCopy = GPUBarrier::Image(&videoTexture_, ResourceState::SHADER_RESOURCE, ResourceState::COPY_DST);
    device->Barrier(&toCopy, 1, cmd);
    device->CopyTexture(&videoTexture_, 0, 0, 0, 0, 0, &staging, 0, 0, cmd);
    const GPUBarrier toRead = GPUBarrier::Image(&videoTexture_, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE);
    device->Barrier(&toRead, 1, cmd);
    videoFrameReady_ = true;
}

bool StartupSplash::LoadImage(const std::string& path) {
    // Same file the SDL splash in st::Run shows before the device exists, read the same
    // way: loose first, then out of the package (FileRead resolves through the packs).
    std::vector<uint8_t> bytes;
    if (!wi::helper::FileRead(path, bytes) || bytes.empty()) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (rw == nullptr) return false;
    SDL_Surface* loaded = SDL_LoadBMP_RW(rw, 1);   // 1 = SDL closes the RWops
    if (loaded == nullptr) return false;
    // ABGR8888 is a packed format: on a little-endian machine its bytes are R, G, B, A.
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(loaded);
    if (rgba == nullptr) return false;

    std::vector<uint32_t> pixels(size_t(rgba->w) * size_t(rgba->h));
    for (int y = 0; y < rgba->h; ++y) {
        const uint8_t* row = (const uint8_t*)rgba->pixels + size_t(y) * rgba->pitch;
        std::memcpy(pixels.data() + size_t(y) * rgba->w, row, size_t(rgba->w) * 4);
    }
    for (uint32_t& p : pixels) p |= 0xFF000000u;   // a BMP's alpha is usually zero
    imageWidth_ = uint32_t(rgba->w);
    imageHeight_ = uint32_t(rgba->h);
    SDL_FreeSurface(rgba);

    TextureDesc td;
    td.width = imageWidth_;
    td.height = imageHeight_;
    td.format = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::SHADER_RESOURCE;
    td.usage = Usage::DEFAULT;
    td.layout = ResourceState::SHADER_RESOURCE;
    SubresourceData init;
    init.data_ptr = pixels.data();
    init.row_pitch = imageWidth_ * 4;
    if (!GetDevice()->CreateTexture(&td, &init, &imageTexture_)) return false;
    GetDevice()->SetName(&imageTexture_, "st::StartupSplash::image");
    return true;
}

void StartupSplash::Draw(const wi::Canvas& canvas, ColorSpace colorspace, CommandList cmd) {
    if (phase_ == Phase::Off) return;

    const float cw = canvas.GetLogicalWidth();
    const float ch = canvas.GetLogicalHeight();
    const bool linear = colorspace != ColorSpace::SRGB;
    const float opacity = std::clamp(fade_, 0.0f, 1.0f);

    wi::image::Params background;
    background.enableFullScreen();
    background.color = XMFLOAT4(settings_.background.x, settings_.background.y, settings_.background.z, 1.0f);
    background.opacity = opacity;
    if (linear) background.enableLinearOutputMapping(9);
    wi::image::Draw(nullptr, background, cmd);

    const Texture* picture = nullptr;
    float w = 0.0f, h = 0.0f;
    if (videoFrameReady_ && videoTexture_.IsValid()) {
        // The video is fitted inside the window and letterboxed on the background colour,
        // never cropped: the logo must survive a 21:9 or a 4:3 screen.
        picture = &videoTexture_;
        const float aspect = float(video_.Width()) / float(std::max(1u, video_.Height()));
        w = cw;
        h = cw / aspect;
        if (h > ch) { h = ch; w = ch * aspect; }
    } else if (imageTexture_.IsValid()) {
        // The image at its own pixel size, as the SDL splash showed it, so the hand-over
        // from that one does not jump - only shrunk if the window is smaller.
        picture = &imageTexture_;
        w = canvas.PhysicalToLogical(float(imageWidth_));
        h = canvas.PhysicalToLogical(float(imageHeight_));
        const float scale = std::min(1.0f, std::min(cw / std::max(1.0f, w), ch / std::max(1.0f, h)));
        w *= scale;
        h *= scale;
    }
    if (picture != nullptr) {
        wi::image::Params fx;
        fx.pos = XMFLOAT3(cw * 0.5f, ch * 0.5f, 0.0f);
        fx.siz = XMFLOAT2(w, h);
        fx.pivot = XMFLOAT2(0.5f, 0.5f);
        fx.opacity = opacity;
        if (linear) fx.enableLinearOutputMapping(9);
        wi::image::Draw(picture, fx, cmd);
    }

    // Only once the wait is long enough to wonder about.
    if (phase_ == Phase::Hold && holdElapsed_ > 0.75f && !status_.empty()) {
        wi::font::Params fp;
        fp.posX = cw * 0.5f;
        fp.posY = ch - 28.0f;
        fp.h_align = wi::font::WIFALIGN_CENTER;
        fp.v_align = wi::font::WIFALIGN_BOTTOM;
        fp.size = 16;
        fp.color = wi::Color(255, 255, 255, 170);
        fp.shadowColor = wi::Color(0, 0, 0, 170);
        if (linear) fp.enableLinearOutputMapping(9);
        wi::font::Draw(status_, fp, cmd);
    }
}

void StartupSplash::Shutdown() {
    phase_ = Phase::Off;
    audio_.reset();
    video_.Close();
    videoDirty_ = false;
    videoFrameReady_ = false;
    videoTexture_ = {};
    videoStaging_.clear();
    imageTexture_ = {};
    imageWidth_ = imageHeight_ = 0;
}

} // namespace st::render::detroit
