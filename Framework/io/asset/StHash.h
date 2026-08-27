#pragma once
// XXH64 — the one content hash the asset pipeline uses.
//
// Header only, no dependencies beyond <cstdint>/<cstddef>, because the build-time
// packer (tools/stpack) links this and NOTHING else from the engine: it has to be
// compilable before any target exists, the same way the .stpd descriptor bootstrap is.
//
// Why XXH64 and not a CRC or std::hash:
//   - ~5-8 GB/s scalar, which means hashing a 50 MB pack part is noise next to the
//     disk read it just did. A CRC32 is comparably fast but 32 bits collide within a
//     few tens of thousands of assets, and asset IDs ARE the hash here.
//   - std::hash is not stable across compilers or runs, so it cannot key a file
//     format at all.
//   - The engine's wi::helper::HashByteData() combines byte-at-a-time, which is
//     roughly two orders of magnitude slower; it is fine for a runtime cache key and
//     unusable for verifying a pack.
//
// It is NOT a cryptographic hash. It answers "did this file rot" and "which asset is
// this", not "did someone tamper with this".

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

namespace st::asset {

namespace detail {

constexpr uint64_t XXH_P1 = 11400714785074694791ULL;
constexpr uint64_t XXH_P2 = 14029467366897019727ULL;
constexpr uint64_t XXH_P3 =  1609587929392839161ULL;
constexpr uint64_t XXH_P4 =  9650029242287828579ULL;
constexpr uint64_t XXH_P5 =  2870177450012600261ULL;

inline uint64_t rotl64 (uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t read64 (const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));   // the format is little-endian and so is every
    return v;                        // platform this engine targets; no swap needed
}
inline uint32_t read32 (const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint64_t round (uint64_t acc, uint64_t input) {
    acc += input * XXH_P2;
    acc  = rotl64(acc, 31);
    acc *= XXH_P1;
    return acc;
}

inline uint64_t mergeRound (uint64_t acc, uint64_t val) {
    val = round(0, val);
    acc ^= val;
    acc  = acc * XXH_P1 + XXH_P4;
    return acc;
}

} // namespace detail

// One-shot XXH64 over a byte range.
inline uint64_t Hash64 (const void* data, size_t size, uint64_t seed = 0) {
    using namespace detail;
    const uint8_t* p   = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + size;
    uint64_t h64;

    if (size >= 32) {
        const uint8_t* limit = end - 32;
        uint64_t v1 = seed + XXH_P1 + XXH_P2;
        uint64_t v2 = seed + XXH_P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - XXH_P1;
        do {
            v1 = round(v1, read64(p)); p += 8;
            v2 = round(v2, read64(p)); p += 8;
            v3 = round(v3, read64(p)); p += 8;
            v4 = round(v4, read64(p)); p += 8;
        } while (p <= limit);

        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h64 = mergeRound(h64, v1);
        h64 = mergeRound(h64, v2);
        h64 = mergeRound(h64, v3);
        h64 = mergeRound(h64, v4);
    } else {
        h64 = seed + XXH_P5;
    }

    h64 += static_cast<uint64_t>(size);

    while (p + 8 <= end) {
        h64 ^= round(0, read64(p));
        h64  = rotl64(h64, 27) * XXH_P1 + XXH_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h64 ^= static_cast<uint64_t>(read32(p)) * XXH_P1;
        h64  = rotl64(h64, 23) * XXH_P2 + XXH_P3;
        p += 4;
    }
    while (p < end) {
        h64 ^= static_cast<uint64_t>(*p) * XXH_P5;
        h64  = rotl64(h64, 11) * XXH_P1;
        ++p;
    }

    h64 ^= h64 >> 33; h64 *= XXH_P2;
    h64 ^= h64 >> 29; h64 *= XXH_P3;
    h64 ^= h64 >> 32;
    return h64;
}

// Streaming form, for hashing a file that is not all in memory at once. Feed the same
// bytes in the same order as Hash64() and you get the same value.
class Hasher64 {
public:
    explicit Hasher64 (uint64_t seed = 0) { Reset(seed); }

    void Reset (uint64_t seed = 0) {
        using namespace detail;
        v1_ = seed + XXH_P1 + XXH_P2;
        v2_ = seed + XXH_P2;
        v3_ = seed;
        v4_ = seed - XXH_P1;
        seed_ = seed;
        total_ = 0;
        held_ = 0;
    }

    void Update (const void* data, size_t size) {
        using namespace detail;
        const uint8_t* p   = static_cast<const uint8_t*>(data);
        const uint8_t* end = p + size;
        total_ += size;

        if (held_ + size < 32) {                       // still short of one stripe
            std::memcpy(buf_ + held_, p, size);
            held_ += size;
            return;
        }
        if (held_ > 0) {                               // finish the held stripe first
            const size_t fill = 32 - held_;
            std::memcpy(buf_ + held_, p, fill);
            const uint8_t* b = buf_;
            v1_ = round(v1_, read64(b)); b += 8;
            v2_ = round(v2_, read64(b)); b += 8;
            v3_ = round(v3_, read64(b)); b += 8;
            v4_ = round(v4_, read64(b));
            p += fill;
            held_ = 0;
        }
        if (p + 32 <= end) {
            const uint8_t* limit = end - 32;
            do {
                v1_ = round(v1_, read64(p)); p += 8;
                v2_ = round(v2_, read64(p)); p += 8;
                v3_ = round(v3_, read64(p)); p += 8;
                v4_ = round(v4_, read64(p)); p += 8;
            } while (p <= limit);
        }
        if (p < end) {
            held_ = static_cast<size_t>(end - p);
            std::memcpy(buf_, p, held_);
        }
    }

    uint64_t Digest () const {
        using namespace detail;
        uint64_t h64;
        if (total_ >= 32) {
            h64 = rotl64(v1_, 1) + rotl64(v2_, 7) + rotl64(v3_, 12) + rotl64(v4_, 18);
            h64 = mergeRound(h64, v1_);
            h64 = mergeRound(h64, v2_);
            h64 = mergeRound(h64, v3_);
            h64 = mergeRound(h64, v4_);
        } else {
            h64 = seed_ + XXH_P5;
        }
        h64 += total_;

        const uint8_t* p   = buf_;
        const uint8_t* end = buf_ + held_;
        while (p + 8 <= end) {
            h64 ^= round(0, read64(p));
            h64  = rotl64(h64, 27) * XXH_P1 + XXH_P4;
            p += 8;
        }
        if (p + 4 <= end) {
            h64 ^= static_cast<uint64_t>(read32(p)) * XXH_P1;
            h64  = rotl64(h64, 23) * XXH_P2 + XXH_P3;
            p += 4;
        }
        while (p < end) {
            h64 ^= static_cast<uint64_t>(*p) * XXH_P5;
            h64  = rotl64(h64, 11) * XXH_P1;
            ++p;
        }

        h64 ^= h64 >> 33; h64 *= XXH_P2;
        h64 ^= h64 >> 29; h64 *= XXH_P3;
        h64 ^= h64 >> 32;
        return h64;
    }

private:
    uint64_t v1_ = 0, v2_ = 0, v3_ = 0, v4_ = 0;
    uint64_t seed_  = 0;
    uint64_t total_ = 0;
    uint8_t  buf_[32] = {};
    size_t   held_ = 0;
};

// Logical asset paths, and the ID derived from one.
//
// Two different normalisations, on purpose:
//
//   NormalizePath   forward slashes, no "./", no leading slash, no doubled slashes.
//                   CASE IS PRESERVED. This is what gets STORED, because the engine's
//                   own resource names are case sensitive on Linux and a material
//                   deserialised from a scene asks for the exact string it saved. Lower
//                   casing the stored name would make a converted map un-reversible and
//                   would break the extractor's output on a case sensitive filesystem.
//
//   CanonicalPath   NormalizePath, then lower cased. This is what gets HASHED, so a
//                   lookup succeeds whether the caller wrote "Textures/Wall.DDS" or
//                   "textures/wall.dds" — the two spellings differ between Windows and
//                   Linux authoring far more often than they mean two different files.
//
// An asset ID is XXH64 of the canonical path, so it is reproducible from the path alone
// on any machine: nothing has to be handed out, remembered, or kept in sync.

inline std::string NormalizePath (const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c == '/' && !out.empty() && out.back() == '/') continue;   // collapse //
        out.push_back(c);
    }
    size_t start = 0;
    while (start + 1 < out.size() && out[start] == '.' && out[start + 1] == '/') start += 2;
    while (start < out.size() && out[start] == '/') ++start;
    if (start) out.erase(0, start);
    return out;
}

inline std::string CanonicalPath (const std::string& path) {
    std::string out = NormalizePath(path);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

inline uint64_t AssetIdFromPath (const std::string& path) {
    const std::string c = CanonicalPath(path);
    return Hash64(c.data(), c.size());
}

} // namespace st::asset
