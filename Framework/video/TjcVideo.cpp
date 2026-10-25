#include "video/TjcVideo.h"

// The one translation unit that compiles the codec. Decoder only: the encoder is the
// tjc tool's business, and leaving it out also leaves out its <thread> pool.
#define TJC_NO_ENCODER
#define TJC_IMPLEMENTATION
#include "video/tjc.h"

#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiJobSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Pull-style input over a byte block the caller keeps alive.
struct ByteReader {
    const std::vector<uint8_t>* bytes = nullptr;
    size_t pos = 0;

    static size_t Read(void* user, void* dst, size_t n) {
        ByteReader* r = static_cast<ByteReader*>(user);
        const size_t left = r->bytes->size() - r->pos;
        const size_t take = n < left ? n : left;
        std::memcpy(dst, r->bytes->data() + r->pos, take);
        r->pos += take;
        return take;
    }
};

// A QOA frame header: channels (1), rate (3), samples per channel (2), frame size (2),
// big-endian like the rest of QOA.
bool ReadQoaFrameHeader(const uint8_t* p, size_t left, int& channels, uint32_t& rate,
                        uint32_t& samples, uint32_t& size) {
    if (left < 8) return false;
    channels = p[0];
    rate     = (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    samples  = (uint32_t(p[4]) << 8) | p[5];
    size     = (uint32_t(p[6]) << 8) | p[7];
    return channels > 0 && size > 8 && size <= left;
}

// The TJC audio track as an st::audio decoder: the QOA frames one after another, read
// in order. Each frame carries its own predictor state, so no frame depends on the one
// before - but they are short and of varying length, so a seek would be a scan, and a
// splash never seeks.
class TjcAudioDecoder final : public st::audio::IAudioDecoder {
public:
    TjcAudioDecoder(std::vector<uint8_t>&& frames, int channels, uint32_t rate, int64_t samples)
        : frames_(std::move(frames)), channels_(channels), rate_(rate), samples_(samples),
          scratch_(size_t(tjc::detail::qoa::kMaxFrameLen) * size_t(channels)) {}

    st::audio::AudioFormat GetFormat() const override { return st::audio::AudioFormat::Qoa; }
    int GetChannels() const override { return channels_; }
    int GetSampleRate() const override { return int(rate_); }
    int64_t GetTotalFrames() const override { return samples_; }

    int ReadFrames(float* out, int frames) override {
        int written = 0;
        while (written < frames) {
            if (cached_ == used_) {
                if (pos_ >= frames_.size()) break;
                size_t consumed = 0;
                const int got = tjc::detail::qoa::decode_frame(frames_.data() + pos_, frames_.size() - pos_,
                                                               channels_, rate_, scratch_.data(), &consumed);
                if (got < 0 || consumed == 0) {
                    pos_ = frames_.size();   // damaged: end the track rather than spin on it
                    break;
                }
                pos_ += consumed;
                cached_ = got;
                used_ = 0;
                continue;
            }
            const int take = std::min(frames - written, cached_ - used_);
            const int16_t* src = scratch_.data() + size_t(used_) * size_t(channels_);
            float* dst = out + size_t(written) * size_t(channels_);
            for (size_t i = 0, n = size_t(take) * size_t(channels_); i < n; ++i)
                dst[i] = float(src[i]) / 32768.0f;
            used_ += take;
            written += take;
        }
        cursor_ += written;
        return written;
    }

    bool Seek(int64_t frame) override {
        if (frame != 0) return false;
        pos_ = 0;
        cached_ = used_ = 0;
        cursor_ = 0;
        return true;
    }
    bool IsSeekable() const override { return false; }
    int64_t GetFrameOffset() const override { return cursor_; }

private:
    std::vector<uint8_t> frames_;
    int channels_ = 0;
    uint32_t rate_ = 0;
    int64_t samples_ = 0;
    size_t pos_ = 0;
    std::vector<int16_t> scratch_;
    int cached_ = 0;   // samples per channel in scratch_
    int used_ = 0;     // of those, already handed out
    int64_t cursor_ = 0;
};

// YUV -> RGB in 16.16 fixed point, limited ("TV") range - what ffmpeg's yuv420p is.
struct Coefficients {
    int y, rv, gu, gv, bu;
};
constexpr Coefficients kBT601 = { 76309, 104597, 25675, 53279, 132201 };
constexpr Coefficients kBT709 = { 76309, 117489, 13975, 34925, 138438 };

inline uint32_t Clamp255(int v) {
    return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

} // namespace

namespace st::video {

TjcVideo::TjcVideo() = default;

TjcVideo::~TjcVideo() {
    Close();
}

bool TjcVideo::Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    Close();
    return false;
}

bool TjcVideo::Open(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!wi::helper::FileRead(path, bytes) || bytes.empty())
        return Fail(error, "cannot read " + path);
    return OpenFromMemory(std::move(bytes), error);
}

bool TjcVideo::OpenFromMemory(std::vector<uint8_t>&& bytes, std::string* error) {
    Close();
    bytes_ = std::move(bytes);

    // Parse-only pass: counts the frames and lifts the audio out. apply = false skips
    // every IDCT and every picture write, so this is a fraction of a real decode.
    {
        tjc::Decoder probe;
        ByteReader reader{ &bytes_, 0 };
        tjc::Status status = probe.read_header(&ByteReader::Read, &reader);
        if (status != tjc::Status::Ok)
            return Fail(error, std::string("not a TJC stream: ") + tjc::status_string(status));

        const tjc::StreamHeader& header = probe.header();
        width_ = header.width;
        height_ = header.height;
        frameRate_ = header.fps_den > 0 ? double(header.fps_num) / double(header.fps_den) : 30.0;
        audioChannels_ = header.audio_codec != 0 ? header.audio_channels : 0;
        audioRate_ = header.audio_codec != 0 ? header.audio_rate : 0;

        tjc::FrameStats stats;
        while ((status = probe.decode_frame(&ByteReader::Read, &reader, &stats, false)) == tjc::Status::Ok) {
            ++frameCount_;
            // The audio is the tail of the frame record, so it ends where the reader is.
            if (stats.audio_bytes > 0 && stats.audio_bytes <= reader.pos)
                audio_.insert(audio_.end(), bytes_.begin() + (reader.pos - stats.audio_bytes),
                              bytes_.begin() + reader.pos);
        }
        if (status != tjc::Status::EndOfStream)
            wi::backlog::post(std::string("TJC: stream ends early (") + tjc::status_string(status) + "), playing " +
                              std::to_string(frameCount_) + " frames", wi::backlog::LogLevel::Warning);
        if (frameCount_ == 0)
            return Fail(error, "TJC stream holds no frames");
    }

    // Count the samples, which the stream does not state anywhere.
    for (size_t pos = 0; pos < audio_.size();) {
        int channels = 0;
        uint32_t rate = 0, samples = 0, size = 0;
        if (!ReadQoaFrameHeader(audio_.data() + pos, audio_.size() - pos, channels, rate, samples, size)) {
            audio_.resize(pos);   // keep what parsed
            break;
        }
        audioSamples_ += samples;
        pos += size;
    }

    decoder_ = new tjc::Decoder();
    ByteReader reader{ &bytes_, 0 };
    if (decoder_->read_header(&ByteReader::Read, &reader) != tjc::Status::Ok)
        return Fail(error, "TJC header did not re-read");
    readPos_ = reader.pos;

    pixels_.assign(size_t(width_) * height_, 0xFF000000u);
    dirty_.assign(size_t(decoder_->layout().tiles), 0);
    decodedFrame_ = -1;
    finished_ = false;
    return true;
}

void TjcVideo::Close() {
    delete decoder_;
    decoder_ = nullptr;
    bytes_ = {};
    readPos_ = 0;
    width_ = height_ = 0;
    frameCount_ = 0;
    frameRate_ = 0.0;
    decodedFrame_ = -1;
    finished_ = false;
    pixels_ = {};
    dirty_ = {};
    dirtyList_ = {};
    audio_ = {};
    audioChannels_ = 0;
    audioRate_ = 0;
    audioSamples_ = 0;
}

bool TjcVideo::AdvanceTo(double seconds, int maxDecodes) {
    if (decoder_ == nullptr || finished_) return false;

    int target = int(std::floor(std::max(0.0, seconds) * frameRate_));
    target = std::min(target, int(frameCount_) - 1);

    bool changed = false;
    ByteReader reader{ &bytes_, readPos_ };
    for (int n = 0; decodedFrame_ < target && n < maxDecodes; ++n) {
        tjc::FrameStats stats;
        const tjc::Status status = decoder_->decode_frame(&ByteReader::Read, &reader, &stats, true);
        if (status != tjc::Status::Ok) {
            finished_ = true;
            break;
        }
        ++decodedFrame_;
        changed = true;
        const uint8_t* flags = decoder_->dirty_flags();
        for (size_t t = 0; t < dirty_.size(); ++t)
            dirty_[t] |= (stats.force_refresh || flags[t]) ? 1 : 0;
    }
    readPos_ = reader.pos;
    if (decodedFrame_ >= int(frameCount_) - 1) finished_ = true;

    if (changed) ConvertTiles();
    return changed;
}

void TjcVideo::ConvertTiles() {
    const tjc::Layout& layout = decoder_->layout();
    dirtyList_.clear();
    for (size_t t = 0; t < dirty_.size(); ++t) {
        if (dirty_[t]) dirtyList_.push_back(int(t));
        dirty_[t] = 0;
    }
    if (dirtyList_.empty()) return;

    const Coefficients& k = matrix_ == ColorMatrix::BT601 ? kBT601
                          : matrix_ == ColorMatrix::BT709 ? kBT709
                          : (height_ >= 720 ? kBT709 : kBT601);
    // A chroma sample belongs to the tile holding its top-left luma pixel, so with odd
    // tile sizes a pixel on a tile edge can read chroma another tile just wrote. One
    // pixel of margin covers it.
    const int margin = ((layout.tile_w | layout.tile_h) & 1) ? 1 : 0;
    const uint8_t* planeY = decoder_->plane(0);
    const uint8_t* planeU = decoder_->plane(1);
    const uint8_t* planeV = decoder_->plane(2);
    const int strideY = decoder_->stride(0);
    const int strideC = decoder_->stride(1);
    const int w = int(width_), h = int(height_);
    uint32_t* out = pixels_.data();

    auto convertTile = [&](int tile) {
        const int x0 = std::max(0, (tile % layout.cols) * layout.tile_w - margin);
        const int y0 = std::max(0, (tile / layout.cols) * layout.tile_h - margin);
        const int x1 = std::min(w, (tile % layout.cols + 1) * layout.tile_w + margin);
        const int y1 = std::min(h, (tile / layout.cols + 1) * layout.tile_h + margin);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* rowY = planeY + size_t(y) * strideY;
            const uint8_t* rowU = planeU + size_t(y >> 1) * strideC;
            const uint8_t* rowV = planeV + size_t(y >> 1) * strideC;
            uint32_t* dst = out + size_t(y) * w;
            for (int x = x0; x < x1; ++x) {
                const int luma = (int(rowY[x]) - 16) * k.y + 32768;
                const int u = int(rowU[x >> 1]) - 128;
                const int v = int(rowV[x >> 1]) - 128;
                const uint32_t r = Clamp255((luma + k.rv * v) >> 16);
                const uint32_t g = Clamp255((luma - k.gu * u - k.gv * v) >> 16);
                const uint32_t b = Clamp255((luma + k.bu * u) >> 16);
                dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
    };

    // A logo that barely moves changes a handful of tiles; spreading those over the
    // job system costs more than converting them here.
    if (dirtyList_.size() < 32) {
        for (int tile : dirtyList_) convertTile(tile);
        return;
    }
    wi::jobsystem::context ctx;
    wi::jobsystem::Dispatch(ctx, uint32_t(dirtyList_.size()), 16, [&](wi::jobsystem::JobArgs args) {
        convertTile(dirtyList_[args.jobIndex]);
    });
    wi::jobsystem::Wait(ctx);
}

st::audio::AudioDecoderPtr TjcVideo::CreateAudioDecoder() const {
    if (audioSamples_ <= 0 || audioChannels_ <= 0 || audioRate_ == 0) return nullptr;
    std::vector<uint8_t> copy = audio_;
    return std::make_unique<TjcAudioDecoder>(std::move(copy), audioChannels_, audioRate_, audioSamples_);
}

} // namespace st::video
