#include "io/Nbt.h"

#include <cstring>   // memcpy
#include <filesystem>
#include <fstream>
#include <system_error>

namespace st::nbt {

// ── factories ───────────────────────────────────────────────────────────────────
Tag Tag::Byte(int8_t v)   { Tag t; t.type = Type::Byte;   t.i = v; return t; }
Tag Tag::Short(int16_t v) { Tag t; t.type = Type::Short;  t.i = v; return t; }
Tag Tag::Int(int32_t v)   { Tag t; t.type = Type::Int;    t.i = v; return t; }
Tag Tag::Long(int64_t v)  { Tag t; t.type = Type::Long;   t.i = v; return t; }
Tag Tag::Float(float v)   { Tag t; t.type = Type::Float;  t.d = v; return t; }
Tag Tag::Double(double v) { Tag t; t.type = Type::Double; t.d = v; return t; }
Tag Tag::Str(std::string v)            { Tag t; t.type = Type::String;    t.str = std::move(v);       return t; }
Tag Tag::Bytes(std::vector<int8_t> v)  { Tag t; t.type = Type::ByteArray; t.byteArray = std::move(v); return t; }
Tag Tag::Ints(std::vector<int32_t> v)  { Tag t; t.type = Type::IntArray;  t.intArray = std::move(v);  return t; }
Tag Tag::Longs(std::vector<int64_t> v) { Tag t; t.type = Type::LongArray; t.longArray = std::move(v); return t; }
Tag Tag::List(Type elementType)        { Tag t; t.type = Type::List;      t.listType = elementType;   return t; }
Tag Tag::Compound()                    { Tag t; t.type = Type::Compound;                              return t; }

// ── compound access ───────────────────────────────────────────────────────────────
Tag& Tag::put(const std::string& name, Tag value) {
    // Children are only serialized for a Compound, so putting into a tag of any other
    // type would silently discard the write at save time. Retype instead: the caller
    // asked for compound behaviour, and the stale scalar payload is what's wrong here
    // (this is the recovery path when a corrupt file gives a section the wrong type).
    if (type != Type::Compound) {
        *this = Compound();
    }
    for (auto& kv : children) {
        if (kv.first == name) { kv.second = std::move(value); return kv.second; }
    }
    children.emplace_back(name, std::move(value));
    return children.back().second;
}
const Tag* Tag::get(const std::string& name) const {
    for (const auto& kv : children)
        if (kv.first == name) return &kv.second;
    return nullptr;
}
Tag* Tag::get(const std::string& name) {
    for (auto& kv : children)
        if (kv.first == name) return &kv.second;
    return nullptr;
}
bool Tag::has(const std::string& name) const { return get(name) != nullptr; }
bool Tag::erase(const std::string& name) {
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].first == name) { children.erase(children.begin() + i); return true; }
    }
    return false;
}

int8_t  Tag::getByte  (const std::string& n, int8_t  def) const { const Tag* t = get(n); return t ? (int8_t)t->asLong(def)  : def; }
int16_t Tag::getShort (const std::string& n, int16_t def) const { const Tag* t = get(n); return t ? (int16_t)t->asLong(def) : def; }
int32_t Tag::getInt   (const std::string& n, int32_t def) const { const Tag* t = get(n); return t ? (int32_t)t->asLong(def) : def; }
int64_t Tag::getLong  (const std::string& n, int64_t def) const { const Tag* t = get(n); return t ? t->asLong(def)          : def; }
float   Tag::getFloat (const std::string& n, float   def) const { const Tag* t = get(n); return t ? (float)t->asDouble(def) : def; }
double  Tag::getDouble(const std::string& n, double  def) const { const Tag* t = get(n); return t ? t->asDouble(def)        : def; }
std::string Tag::getString(const std::string& n, const std::string& def) const { const Tag* t = get(n); return t ? t->asString(def) : def; }
bool    Tag::getBool  (const std::string& n, bool    def) const { const Tag* t = get(n); return t ? (t->asLong(def ? 1 : 0) != 0) : def; }

// ── list access ─────────────────────────────────────────────────────────────────
Tag& Tag::add(Tag value) {
    if (type != Type::List)        // as with put(): items only round-trip on a List
        *this = List(value.type);
    if (items.empty())
        listType = value.type;     // first element fixes the list's element type
    items.push_back(std::move(value));
    return items.back();
}

// ── scalar reads ──────────────────────────────────────────────────────────────────
int64_t Tag::asLong(int64_t def) const {
    switch (type) {
        case Type::Byte: case Type::Short: case Type::Int: case Type::Long: return i;
        case Type::Float: case Type::Double:                                return (int64_t)d;
        default:                                                            return def;
    }
}
double Tag::asDouble(double def) const {
    switch (type) {
        case Type::Float: case Type::Double:                                return d;
        case Type::Byte: case Type::Short: case Type::Int: case Type::Long: return (double)i;
        default:                                                            return def;
    }
}
std::string Tag::asString(const std::string& def) const {
    return type == Type::String ? str : def;
}

// ── serialization (big-endian) ──────────────────────────────────────────────────
namespace {

void w8 (std::vector<uint8_t>& o, uint8_t v)  { o.push_back(v); }
void w16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void w32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s)); }
void w64(std::vector<uint8_t>& o, uint64_t v) { for (int s = 56; s >= 0; s -= 8) o.push_back(uint8_t(v >> s)); }
void wstr(std::vector<uint8_t>& o, const std::string& s) {
    // NBT strings use an unsigned-short length prefix; clamp pathologically long strings.
    const uint16_t len = (uint16_t)(s.size() > 0xFFFF ? 0xFFFF : s.size());
    w16(o, len);
    o.insert(o.end(), s.begin(), s.begin() + len);
}
uint32_t f2b(float f)  { uint32_t b; std::memcpy(&b, &f, 4); return b; }
uint64_t d2b(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

void writePayload(std::vector<uint8_t>& o, const Tag& t) {
    switch (t.type) {
        case Type::End:    break;
        case Type::Byte:   w8 (o, (uint8_t)(int8_t)t.i); break;
        case Type::Short:  w16(o, (uint16_t)(int16_t)t.i); break;
        case Type::Int:    w32(o, (uint32_t)(int32_t)t.i); break;
        case Type::Long:   w64(o, (uint64_t)t.i); break;
        case Type::Float:  w32(o, f2b((float)t.d)); break;
        case Type::Double: w64(o, d2b(t.d)); break;
        case Type::ByteArray:
            w32(o, (uint32_t)t.byteArray.size());
            for (int8_t b : t.byteArray) w8(o, (uint8_t)b);
            break;
        case Type::String: wstr(o, t.str); break;
        case Type::List: {
            const Type et = t.items.empty() ? t.listType : t.items.front().type;
            w8(o, (uint8_t)et);
            w32(o, (uint32_t)t.items.size());
            for (const Tag& e : t.items) writePayload(o, e);
            break;
        }
        case Type::Compound:
            for (const auto& kv : t.children) {
                w8(o, (uint8_t)kv.second.type);
                wstr(o, kv.first);
                writePayload(o, kv.second);
            }
            w8(o, (uint8_t)Type::End);
            break;
        case Type::IntArray:
            w32(o, (uint32_t)t.intArray.size());
            for (int32_t v : t.intArray) w32(o, (uint32_t)v);
            break;
        case Type::LongArray:
            w32(o, (uint32_t)t.longArray.size());
            for (int64_t v : t.longArray) w64(o, (uint64_t)v);
            break;
    }
}

// A list writes ONE element-type header for all its payloads, so a list holding mixed
// types serializes to something no reader (including ours) can parse back. Refuse to
// emit that file rather than hand the user a save that fails to load later.
bool validate(const Tag& t) {
    if (t.type == Type::List) {
        for (const Tag& e : t.items)
            if (e.type != t.items.front().type) return false;
    }
    for (const Tag& e : t.items)
        if (!validate(e)) return false;
    for (const auto& kv : t.children)
        if (!validate(kv.second)) return false;
    return true;
}

} // namespace

bool write(const Tag& root, const std::string& rootName, std::vector<uint8_t>& out) {
    if (root.type != Type::Compound)
        return false; // NBT documents are always rooted at a single named compound
    if (!validate(root))
        return false;
    w8(out, (uint8_t)Type::Compound);
    wstr(out, rootName);
    writePayload(out, root);
    return true;
}

// ── parsing (bounds-checked, never throws) ────────────────────────────────────────
namespace {

struct Cur { const uint8_t* p; size_t n; bool ok = true; };

uint8_t  r8 (Cur& c) { if (c.n < 1) { c.ok = false; return 0; } c.n -= 1; return *c.p++; }
uint16_t r16(Cur& c) { uint16_t hi = r8(c); uint16_t lo = r8(c); return (uint16_t)((hi << 8) | lo); }
uint32_t r32(Cur& c) { uint32_t hi = r16(c); uint32_t lo = r16(c); return (hi << 16) | lo; }
uint64_t r64(Cur& c) { uint64_t hi = r32(c); uint64_t lo = r32(c); return (hi << 32) | lo; }
std::string rstr(Cur& c) {
    const uint16_t len = r16(c);
    if (c.n < len) { c.ok = false; return std::string(); }
    std::string s((const char*)c.p, len);
    c.p += len; c.n -= len;
    return s;
}
float  b2f(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
double b2d(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

// A corrupt/hostile file must not be able to take the process down. Exceptions are
// disabled project-wide, so an allocation failure or a blown stack is fatal rather than
// catchable — the parser has to refuse impossible input up front instead.
//
// Nested compounds cost only 3 bytes per level on the wire (type id + 2-byte name
// length), so an unbounded readPayload recursion overflows the default 1 MB stack on a
// ~100 KB file. Real data nests a handful of levels deep.
constexpr int kMaxDepth = 512;

void readPayload(Cur& c, Type type, Tag& out, int depth) {
    out = Tag();
    out.type = type;
    if (depth > kMaxDepth) { c.ok = false; return; }
    switch (type) {
        case Type::End:    break;
        case Type::Byte:   out.i = (int8_t)r8(c); break;
        case Type::Short:  out.i = (int16_t)r16(c); break;
        case Type::Int:    out.i = (int32_t)r32(c); break;
        case Type::Long:   out.i = (int64_t)r64(c); break;
        case Type::Float:  out.d = b2f(r32(c)); break;
        case Type::Double: out.d = b2d(r64(c)); break;
        case Type::ByteArray: {
            const uint32_t len = r32(c);
            if (len > c.n) { c.ok = false; break; }       // each element is 1 byte
            out.byteArray.resize(len);
            for (uint32_t k = 0; k < len; ++k) out.byteArray[k] = (int8_t)r8(c);
            break;
        }
        case Type::String: out.str = rstr(c); break;
        case Type::List: {
            const uint8_t et = r8(c);
            const uint32_t len = r32(c);
            if (!c.ok) break;
            // An End-typed list has no payload per element, so a bogus length would spin
            // for up to 2^32 iterations appending empty tags until the process dies.
            if (et == (uint8_t)Type::End && len > 0) { c.ok = false; break; }
            // Every other element type consumes at least one byte, so a length past the
            // remaining input is unsatisfiable — reject before reserving for it.
            if (len > c.n) { c.ok = false; break; }
            out.listType = (Type)et;
            // Bounded by input size, but a Tag is far larger than the byte that admits it —
            // grow on demand rather than trusting `len` with one big up-front allocation.
            out.items.reserve(len < 4096 ? len : 4096);
            for (uint32_t k = 0; k < len && c.ok; ++k) {
                Tag e;
                readPayload(c, (Type)et, e, depth + 1);
                if (!c.ok) break;
                out.items.push_back(std::move(e));
            }
            break;
        }
        case Type::Compound: {
            for (;;) {
                const uint8_t t = r8(c);
                if (!c.ok || t == (uint8_t)Type::End) break;
                std::string name = rstr(c);
                if (!c.ok) break;
                Tag child;
                readPayload(c, (Type)t, child, depth + 1);
                if (!c.ok) break;
                out.children.emplace_back(std::move(name), std::move(child));
            }
            break;
        }
        case Type::IntArray: {
            const uint32_t len = r32(c);
            if ((uint64_t)len * 4 > c.n) { c.ok = false; break; }
            out.intArray.resize(len);
            for (uint32_t k = 0; k < len; ++k) out.intArray[k] = (int32_t)r32(c);
            break;
        }
        case Type::LongArray: {
            const uint32_t len = r32(c);
            if ((uint64_t)len * 8 > c.n) { c.ok = false; break; }
            out.longArray.resize(len);
            for (uint32_t k = 0; k < len; ++k) out.longArray[k] = (int64_t)r64(c);
            break;
        }
        default: c.ok = false; break;                     // unknown tag id
    }
}

} // namespace

bool read(const uint8_t* data, size_t size, Tag& outRoot, std::string* outRootName, std::string* error) {
    Cur c{data, size};
    const uint8_t t = r8(c);
    if (!c.ok || t != (uint8_t)Type::Compound) {
        if (error) *error = "NBT: root is not a compound tag";
        return false;
    }
    std::string name = rstr(c);
    readPayload(c, Type::Compound, outRoot, 0);
    if (!c.ok) {
        if (error) *error = "NBT: unexpected end of data (truncated or malformed)";
        return false;
    }
    if (outRootName) *outRootName = std::move(name);
    return true;
}

// ── file helpers ──────────────────────────────────────────────────────────────────
// Written atomically: serialize to a sibling temp file, then rename over the target.
// Writing in place would leave a truncated, unreadable save if the process died (or the
// disk filled) mid-write — with no copy of the previous contents to fall back on.
bool writeFile(const std::string& path, const Tag& root, const std::string& rootName) {
    std::vector<uint8_t> buf;
    if (!write(root, rootName, buf))
        return false;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        if (!buf.empty())
            f.write((const char*)buf.data(), (std::streamsize)buf.size());
        f.close(); // flush before the rename; a failed flush must not be mistaken for success
        if (!f.good()) {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec); // replaces an existing target
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        return false;
    }
    return true;
}
bool readFile(const std::string& path, Tag& outRoot, std::string* outRootName, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        if (error) *error = "NBT: cannot open file: " + path;
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        if (error) *error = "NBT: cannot determine file size: " + path;
        return false;
    }
    std::vector<uint8_t> buf((size_t)size);
    f.seekg(0);
    if (size > 0) {
        f.read((char*)buf.data(), size);
        // A short read sets failbit AND eofbit, so testing good()/eof() would wave it
        // through and parse the uninitialized tail. Compare what actually landed.
        if (f.gcount() != size) {
            if (error) *error = "NBT: read error: " + path;
            return false;
        }
    }
    return read(buf.data(), buf.size(), outRoot, outRootName, error);
}

// ── debug dump (SNBT-ish) ───────────────────────────────────────────────────────
namespace {

const char* typeName(Type t) {
    switch (t) {
        case Type::Byte: return "byte"; case Type::Short: return "short"; case Type::Int: return "int";
        case Type::Long: return "long"; case Type::Float: return "float"; case Type::Double: return "double";
        case Type::ByteArray: return "bytes"; case Type::String: return "string"; case Type::List: return "list";
        case Type::Compound: return "compound"; case Type::IntArray: return "ints"; case Type::LongArray: return "longs";
        default: return "end";
    }
}
void indent(std::string& o, int depth) { for (int k = 0; k < depth; ++k) o += "  "; }

void dumpTag(std::string& o, const std::string& name, const Tag& t, int depth) {
    indent(o, depth);
    if (!name.empty()) o += name + ": ";
    switch (t.type) {
        case Type::Byte: case Type::Short: case Type::Int: case Type::Long:
            o += std::to_string(t.i) + " (" + typeName(t.type) + ")\n"; break;
        case Type::Float: case Type::Double:
            o += std::to_string(t.d) + " (" + typeName(t.type) + ")\n"; break;
        case Type::String:
            o += "\"" + t.str + "\"\n"; break;
        case Type::ByteArray: o += "bytes[" + std::to_string(t.byteArray.size()) + "]\n"; break;
        case Type::IntArray:  o += "ints["  + std::to_string(t.intArray.size())  + "]\n"; break;
        case Type::LongArray: o += "longs[" + std::to_string(t.longArray.size()) + "]\n"; break;
        case Type::List:
            o += "list<" + std::string(typeName(t.listType)) + ">[" + std::to_string(t.items.size()) + "]\n";
            for (const Tag& e : t.items) dumpTag(o, std::string(), e, depth + 1);
            break;
        case Type::Compound:
            o += "{\n";
            for (const auto& kv : t.children) dumpTag(o, kv.first, kv.second, depth + 1);
            indent(o, depth); o += "}\n";
            break;
        default: o += "end\n"; break;
    }
}

} // namespace

std::string dump(const Tag& root, const std::string& rootName) {
    std::string o;
    dumpTag(o, rootName, root, 0);
    return o;
}

} // namespace st::nbt
