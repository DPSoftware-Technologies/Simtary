#pragma once
// Minimal NBT (Named Binary Tag) reader/writer for Milistry. Uncompressed, big-endian,
// matching the classic NBT layout, so files can be inspected with standard NBT tools.
//
// One container format, three file extensions by ROLE — they are not interchangeable,
// and the reader does not care, so a wrong extension fails only at open time:
//
//   .stad   game options       <userdata>/options.stad          (st::SettingsManager)
//   .stcd   save games         <userdata>/saves/<slot>.stcd      (st::savegame)
//   .staod  animation descriptors  assets/animation_descriptor/*.staod (shipped, read-only)
//
// Note .staod is NOT the asset package index — that is .strd (see io/asset/), which is
// a flat binary table, not NBT. The two briefly shared this extension; they no longer do.
//
// The project builds with exceptions and RTTI disabled (/EHsc- /GR-), so this API uses
// NO exceptions and NO RTTI: parsing reports failure via a bool return + optional error
// string, and reads never throw on malformed/truncated input.
//
// A Tag is a self-describing node. A Compound holds named children (kept in insertion
// order); a List holds same-typed unnamed elements. Build a tree, then write it; load it
// back and read fields with defaults.
//
// Example — an animation_descriptor for a character:
//
//   st::nbt::Tag root = st::nbt::Tag::Compound();
//   root.putString("character", "soldier");
//   st::nbt::Tag anims = st::nbt::Tag::List(st::nbt::Type::Compound);
//   for (const Anim& a : list) {
//       st::nbt::Tag c = st::nbt::Tag::Compound();
//       c.putString("id", a.id);
//       c.putString("file", a.file);
//       c.putBool  ("loop", a.loop);
//       c.putFloat ("speed", a.speed);
//       anims.add(std::move(c));
//   }
//   root.put("animations", std::move(anims));
//   st::nbt::writeFile("assets/animation_descriptor/player.staod", root, "animation_descriptor");
//
//   st::nbt::Tag loaded;
//   if (st::nbt::readFile("assets/animation_descriptor/player.staod", loaded)) {
//       std::string who = loaded.getString("character");
//       if (const st::nbt::Tag* l = loaded.get("animations"))
//           for (const st::nbt::Tag& c : l->items) {
//               std::string id = c.getString("id");
//               float speed    = c.getFloat("speed", 1.0f);
//           }
//   }

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace st::nbt {

enum class Type : uint8_t {
    End = 0, Byte, Short, Int, Long, Float, Double,
    ByteArray, String, List, Compound, IntArray, LongArray
};

struct Tag {
    Type type = Type::End;

    // Only the field matching `type` carries data:
    int64_t i = 0;                 // Byte / Short / Int / Long
    double  d = 0.0;               // Float / Double
    std::string str;               // String
    std::vector<int8_t>  byteArray;
    std::vector<int32_t> intArray;
    std::vector<int64_t> longArray;
    Type listType = Type::End;     // element type when type == List
    std::vector<Tag> items;        // List elements
    std::vector<std::pair<std::string, Tag>> children; // Compound entries (insertion ordered)

    Tag() = default;

    // ── factories ─────────────────────────────────────────────────────────────
    static Tag Byte(int8_t v);
    static Tag Short(int16_t v);
    static Tag Int(int32_t v);
    static Tag Long(int64_t v);
    static Tag Float(float v);
    static Tag Double(double v);
    static Tag Str(std::string v);
    static Tag Bytes(std::vector<int8_t> v);
    static Tag Ints(std::vector<int32_t> v);
    static Tag Longs(std::vector<int64_t> v);
    static Tag List(Type elementType);
    static Tag Compound();

    bool isCompound() const { return type == Type::Compound; }
    bool isList()     const { return type == Type::List; }

    // ── compound access (reads return the default when this tag isn't a compound) ──
    // put() retypes a non-compound tag into an empty compound first, discarding its old
    // payload — children are meaningless on any other type and would be dropped on save.
    Tag&       put(const std::string& name, Tag value); // insert or overwrite; returns stored ref
    const Tag* get(const std::string& name) const;      // nullptr if absent
    Tag*       get(const std::string& name);
    bool       has(const std::string& name) const;
    bool       erase(const std::string& name);

    void putByte  (const std::string& name, int8_t  v) { put(name, Byte(v)); }
    void putShort (const std::string& name, int16_t v) { put(name, Short(v)); }
    void putInt   (const std::string& name, int32_t v) { put(name, Int(v)); }
    void putLong  (const std::string& name, int64_t v) { put(name, Long(v)); }
    void putFloat (const std::string& name, float   v) { put(name, Float(v)); }
    void putDouble(const std::string& name, double  v) { put(name, Double(v)); }
    void putString(const std::string& name, std::string v) { put(name, Str(std::move(v))); }
    void putBool  (const std::string& name, bool    v) { put(name, Byte(v ? 1 : 0)); }

    int8_t      getByte  (const std::string& name, int8_t  def = 0) const;
    int16_t     getShort (const std::string& name, int16_t def = 0) const;
    int32_t     getInt   (const std::string& name, int32_t def = 0) const;
    int64_t     getLong  (const std::string& name, int64_t def = 0) const;
    float       getFloat (const std::string& name, float   def = 0.0f) const;
    double      getDouble(const std::string& name, double  def = 0.0) const;
    std::string getString(const std::string& name, const std::string& def = std::string()) const;
    bool        getBool  (const std::string& name, bool    def = false) const;

    // ── list access ─────────────────────────────────────────────────────────────
    // Retypes a non-list tag into a list, mirroring put(). All elements must share one
    // type — a mixed list is unserializable and write() rejects it.
    Tag& add(Tag value); // append to a List; sets the element type from the first element

    // ── scalar reads with default (numeric tags convert between each other) ─────
    int64_t     asLong  (int64_t def = 0) const;
    double      asDouble(double  def = 0.0) const;
    std::string asString(const std::string& def = std::string()) const;
};

// Serialize/parse uncompressed big-endian NBT. `root` must be a Compound tag.
bool write(const Tag& root, const std::string& rootName, std::vector<uint8_t>& out);
bool read (const uint8_t* data, size_t size, Tag& outRoot,
           std::string* outRootName = nullptr, std::string* error = nullptr);

// File helpers (binary). writeFile is atomic — it writes <path>.tmp and renames over the
// target, so an interrupted save never truncates the previous file. Both return false on
// I/O failure; readFile also returns false (with `error` set) on malformed input.
bool writeFile(const std::string& path, const Tag& root, const std::string& rootName = std::string());
bool readFile (const std::string& path, Tag& outRoot,
               std::string* outRootName = nullptr, std::string* error = nullptr);

// Human-readable dump (SNBT-ish) for debugging saves / descriptors.
std::string dump(const Tag& root, const std::string& rootName = std::string());

} // namespace st::nbt
