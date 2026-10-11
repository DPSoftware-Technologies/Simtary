#include "devui/imaudiopreview.h"

#include "imgui.h"
#include "io/asset/StHash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st::devui {

namespace {

// Decoding is whole-file (st::audio::LoadClip keeps PCM), so a long track is decoded only
//	when asked for rather than the moment it is selected.
constexpr uint64_t kAutoDecodeBytes = 16ull * 1024 * 1024;

std::string FormatTime (float seconds) {
    seconds = std::max(0.0f, seconds);
    const int m = (int)(seconds / 60.0f);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%04.1f", m, seconds - m * 60.0f);
    return buffer;
}

} // namespace

AudioPreview::~AudioPreview () {
    if (emitter_ && st::audio::AudioEngine::Get().IsInitialized()) {
        emitter_->Stop();
        st::audio::AudioEngine::Get().Destroy(emitter_);
    }
}

void AudioPreview::Select (uint64_t id, const std::string& loadName, const std::vector<uint8_t>* bytes,
                           uint64_t sizeHint) {
    if (id == id_) return;
    Stop();
    id_ = id;
    name_ = loadName;
    fromMemory_ = bytes != nullptr;
    bytes_ = bytes ? *bytes : std::vector<uint8_t>();
    sizeHint_ = sizeHint;
    clip_.reset();
    loadFailed_ = false;
    startAt_ = 0.0f;
    peakMin_.clear();
    peakMax_.clear();
}

bool AudioPreview::EnsureClip () {
    if (clip_) return true;
    if (loadFailed_ || id_ == 0) return false;
    clip_ = fromMemory_ ? st::audio::LoadClipFromMemory(bytes_.data(), bytes_.size(), name_)
                        : st::audio::LoadClip(name_);
    loadFailed_ = !clip_;
    return !loadFailed_;
}

void AudioPreview::EnsureEmitter () {
    if (emitter_ || !st::audio::AudioEngine::Get().IsInitialized()) return;
    emitter_ = st::audio::AudioEngine::Get().CreateEmitter("Resource Explorer preview");
    emitter_->SetSpatial(false);                 // a preview has no position
    emitter_->SetSubmix(st::audio::Submix::UI);
}

void AudioPreview::Play () {
    if (!EnsureClip()) return;
    EnsureEmitter();
    if (!emitter_) return;
    if (emitter_->IsPaused()) {
        emitter_->Play(); // resume where it paused
        return;
    }
    emitter_->SetClip(clip_);
    emitter_->SetLoop(loop_);
    emitter_->SetVolume(volume_);
    emitter_->Play();
    if (startAt_ > 0.0f) emitter_->SetTime(startAt_);
}

void AudioPreview::Stop () {
    if (emitter_) emitter_->Stop();
}

bool AudioPreview::IsPlaying (uint64_t id) const {
    return id == id_ && emitter_ && emitter_->IsPlaying();
}

void AudioPreview::Toggle (uint64_t id, const std::string& loadName, const std::vector<uint8_t>* bytes) {
    if (IsPlaying(id)) {
        Stop();
        return;
    }
    Select(id, loadName, bytes);
    startAt_ = 0.0f;
    Play();
}

void AudioPreview::BuildPeaks (int columns) {
    peakMin_.assign((size_t)columns, 0.0f);
    peakMax_.assign((size_t)columns, 0.0f);
    if (!clip_ || clip_->channels <= 0) return;
    const int frames = clip_->GetFrameCount();
    const int ch = clip_->channels;
    const float* s = clip_->samples.data();
    for (int c = 0; c < columns; ++c) {
        const int a = (int)((int64_t)frames * c / columns);
        const int b = std::max(a + 1, (int)((int64_t)frames * (c + 1) / columns));
        float lo = 0.0f, hi = 0.0f;
        for (int f = a; f < b && f < frames; ++f) {
            for (int k = 0; k < ch; ++k) {
                const float v = s[(size_t)f * ch + k];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        peakMin_[c] = lo;
        peakMax_[c] = hi;
    }
}

void AudioPreview::DrawInspector () {
    if (id_ == 0) return;
    ImGui::SeparatorText("Audio preview");

    if (!st::audio::AudioEngine::Get().IsInitialized()) {
        ImGui::TextDisabled("Audio engine is not running.");
        return;
    }
    // Decode at once when it is small enough not to stall the click; otherwise on demand.
    if (!clip_ && !loadFailed_ && (sizeHint_ == 0 || sizeHint_ <= kAutoDecodeBytes)) EnsureClip();
    if (loadFailed_) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Could not decode this sound (see backlog).");
        return;
    }
    if (!clip_) {
        if (ImGui::Button("Decode for preview")) EnsureClip();
        ImGui::SameLine();
        ImGui::TextDisabled("large file - decoding takes a moment");
        return;
    }

    const bool playing = emitter_ && emitter_->IsPlaying();
    const bool paused = emitter_ && emitter_->IsPaused();
    const float length = clip_->GetLengthSeconds();
    const float now = (playing || paused) ? emitter_->GetTime() : startAt_;

    // transport
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(60, 0))) {
        if (playing) emitter_->Pause();
        else         Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(60, 0))) {
        Stop();
        startAt_ = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &loop_) && emitter_) emitter_->SetLoop(loop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##vol", &volume_, 0.0f, 1.0f, "vol %.2f") && emitter_) emitter_->SetVolume(volume_);
    ImGui::SameLine();
    ImGui::TextDisabled("%s / %s", FormatTime(now).c_str(), FormatTime(length).c_str());
    ImGui::TextDisabled("%d Hz, %s, %d frames", clip_->sampleRate,
                        clip_->channels == 1 ? "mono" : clip_->channels == 2 ? "stereo" : "multichannel",
                        clip_->GetFrameCount());

    // waveform: min/max per pixel column, played part lit, playhead, click to seek
    const float width = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    const float height = 64.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##waveform", ImVec2(width, height));
    const int columns = std::max(1, (int)width);
    if ((int)peakMin_.size() != columns) BuildPeaks(columns);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(24, 26, 30, 255), 3.0f);
    const float mid = p.y + height * 0.5f;
    const float played = length > 0.0f ? std::clamp(now / length, 0.0f, 1.0f) : 0.0f;
    for (int c = 0; c < columns; ++c) {
        const float x = p.x + c + 0.5f;
        const bool lit = (float)c / columns <= played && (playing || paused || startAt_ > 0.0f);
        const ImU32 col = lit ? IM_COL32(255, 170, 60, 255) : IM_COL32(110, 160, 220, 255);
        const float y0 = mid - std::clamp(peakMax_[c], 0.0f, 1.0f) * (height * 0.48f);
        const float y1 = mid - std::clamp(peakMin_[c], -1.0f, 0.0f) * (height * 0.48f);
        dl->AddLine(ImVec2(x, y0), ImVec2(x, std::max(y1, y0 + 1.0f)), col);
    }
    dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + width, mid), IM_COL32(60, 64, 72, 255));
    const float hx = p.x + played * width;
    dl->AddLine(ImVec2(hx, p.y), ImVec2(hx, p.y + height), IM_COL32(255, 255, 255, 220), 1.5f);

    if (ImGui::IsItemActive() && length > 0.0f) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / width, 0.0f, 1.0f) * length;
        if (playing || paused) emitter_->SetTime(t);
        else                   startAt_ = t;
    }
    if (ImGui::IsItemHovered()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / width, 0.0f, 1.0f) * length;
        ImGui::SetTooltip("%s - click to seek", FormatTime(t).c_str());
    }
}

} // namespace st::devui
