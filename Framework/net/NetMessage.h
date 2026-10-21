#pragma once
// Wire encoding for st::net messages: one type byte, then little-endian fields. Bounds-checked
// reads - a short or malformed message reads as zeros and flips ok() false, never overruns.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace st::net {

class Writer {
public:
    explicit Writer (uint8_t type) { U8(type); }

    void U8 (uint8_t v) { bytes_.push_back(v); }
    void U16 (uint16_t v) { Raw(&v, 2); }
    void U32 (uint32_t v) { Raw(&v, 4); }
    void F32 (float v) { Raw(&v, 4); }
    void F64 (double v) { Raw(&v, 8); }
    void Str (const std::string& s) {
        const uint16_t n = uint16_t(s.size() < 0xFFFF ? s.size() : 0xFFFF);
        U16(n);
        Raw(s.data(), n);
    }

    // u32 length, then the bytes (an archive, a file).
    void Blob (const void* p, size_t n) {
        U32(uint32_t(n));
        Raw(p, n);
    }

    const std::vector<uint8_t>& Bytes () const { return bytes_; }

private:
    // The engine targets little-endian CPUs only (x86-64, arm64), so a memcpy is the encoding.
    void Raw (const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        bytes_.insert(bytes_.end(), b, b + n);
    }
    std::vector<uint8_t> bytes_;
};

class Reader {
public:
    Reader (const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    explicit Reader (const std::vector<uint8_t>& v) : Reader(v.data(), v.size()) {}

    uint8_t U8 () { uint8_t v = 0; Raw(&v, 1); return v; }
    uint16_t U16 () { uint16_t v = 0; Raw(&v, 2); return v; }
    uint32_t U32 () { uint32_t v = 0; Raw(&v, 4); return v; }
    float F32 () { float v = 0; Raw(&v, 4); return v; }
    double F64 () { double v = 0; Raw(&v, 8); return v; }
    std::string Str () {
        const uint16_t n = U16();
        if (!ok_ || size_t(end_ - p_) < n) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }

    // Counterpart of Writer::Blob. Points into the message: valid while it is.
    const uint8_t* Blob (size_t& n) {
        n = U32();
        if (!ok_ || size_t(end_ - p_) < n) { ok_ = false; n = 0; return nullptr; }
        const uint8_t* at = p_;
        p_ += n;
        return at;
    }

    bool ok () const { return ok_; }

private:
    void Raw (void* out, size_t n) {
        if (!ok_ || size_t(end_ - p_) < n) { ok_ = false; return; }
        memcpy(out, p_, n);
        p_ += n;
    }
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

} // namespace st::net
