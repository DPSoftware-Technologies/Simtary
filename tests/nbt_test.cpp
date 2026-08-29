// Standalone round-trip test for the NBT module (src/io/Nbt). Built as its own console
// executable `nbt_test` (see CMakeLists.txt), under the same /EHsc- /GR- flags as the game.
// Returns the number of failed checks (0 = success), so it works as a CTest test too.
#include "io/Nbt.h"

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace st::nbt;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    // Build a tree exercising every tag type + nesting (list of compounds, list of strings).
    Tag root = Tag::Compound();
    root.putString("character", "soldier");
    root.putInt("version", 3);
    root.putLong("bigId", 0x1122334455667788LL);
    root.putShort("s", -1234);
    root.putByte("b", -5);
    root.putFloat("speed", 1.25f);
    root.putDouble("pi", 3.14159265358979);
    root.putBool("loop", true);
    root.put("ints", Tag::Ints({1, -2, 3, -2000000000}));
    root.put("longs", Tag::Longs({1LL, -2LL, 9000000000LL}));
    root.put("bytes", Tag::Bytes({0, 127, -128, 42}));

    Tag anims = Tag::List(Type::Compound);
    for (int k = 0; k < 3; ++k) {
        Tag c = Tag::Compound();
        c.putString("id", std::string("anim_") + std::to_string(k));
        c.putFloat("speed", 0.5f * (k + 1));
        c.putBool("loop", (k % 2) == 0);
        anims.add(std::move(c));
    }
    root.put("animations", std::move(anims));

    Tag strList = Tag::List(Type::String);
    strList.add(Tag::Str("walk"));
    strList.add(Tag::Str("run"));
    root.put("names", std::move(strList));

    // Round-trip through bytes.
    std::vector<uint8_t> buf;
    CHECK(write(root, "animation_descriptor", buf), "write failed");
    CHECK(!buf.empty(), "empty buffer");

    Tag back;
    std::string rootName, err;
    CHECK(read(buf.data(), buf.size(), back, &rootName, &err), err.c_str());
    CHECK(rootName == "animation_descriptor", "root name mismatch");

    CHECK(back.getString("character") == "soldier", "character");
    CHECK(back.getInt("version") == 3, "version");
    CHECK(back.getLong("bigId") == 0x1122334455667788LL, "bigId");
    CHECK(back.getShort("s") == -1234, "short");
    CHECK(back.getByte("b") == -5, "byte");
    CHECK(std::fabs(back.getFloat("speed") - 1.25f) < 1e-6f, "speed");
    CHECK(std::fabs(back.getDouble("pi") - 3.14159265358979) < 1e-12, "pi");
    CHECK(back.getBool("loop") == true, "loop");

    const Tag* ints = back.get("ints");
    CHECK(ints && ints->type == Type::IntArray && ints->intArray.size() == 4 && ints->intArray[3] == -2000000000, "ints");
    const Tag* longs = back.get("longs");
    CHECK(longs && longs->longArray.size() == 3 && longs->longArray[2] == 9000000000LL, "longs");
    const Tag* bytes = back.get("bytes");
    CHECK(bytes && bytes->byteArray.size() == 4 && bytes->byteArray[2] == -128, "bytes");

    const Tag* a = back.get("animations");
    CHECK(a && a->type == Type::List && a->listType == Type::Compound && a->items.size() == 3, "animations list");
    if (a && a->items.size() == 3) {
        CHECK(a->items[1].getString("id") == "anim_1", "anim id");
        CHECK(std::fabs(a->items[1].getFloat("speed") - 1.0f) < 1e-6f, "anim speed");
        CHECK(a->items[2].getBool("loop") == true, "anim2 loop");
    }
    const Tag* names = back.get("names");
    CHECK(names && names->items.size() == 2 && names->items[0].str == "walk" && names->items[1].str == "run", "names");

    // Write a persistent sample file (uncompressed big-endian NBT) that opens in NBTExplorer,
    // then read it back to confirm the on-disk round-trip.
    const std::string path = "nbt_sample.staod";
    CHECK(writeFile(path, root, "animation_descriptor"), "writeFile failed");
    Tag fromFile;
    CHECK(readFile(path, fromFile), "readFile failed");
    CHECK(fromFile.getString("character") == "soldier", "file character");
    std::printf("wrote sample NBT: %s  (open in NBTExplorer: File > Open File)\n", path.c_str());

    // Truncated input must fail gracefully (no crash, no throw).
    Tag t2;
    CHECK(!read(buf.data(), buf.size() / 2, t2, nullptr, nullptr), "truncated should fail");

    // ── malformed input must be rejected, not merely survived ────────────────────
    // Exceptions are off, so an over-allocation or a blown stack terminates the process:
    // each of these used to do exactly that. If the parser regresses, this test crashes
    // rather than reporting a failure — that is the intended signal.

    // A list claiming 2^32-1 End-typed elements. End has no payload, so nothing in the
    // element loop can consume input and stop it.
    {
        const uint8_t bomb[] = {
            0x0A, 0x00, 0x00,             // TAG_Compound, name ""
            0x09, 0x00, 0x01, 'x',        // TAG_List "x"
            0x00,                         // element type = TAG_End
            0xFF, 0xFF, 0xFF, 0xFF,       // length = 4294967295
            0x00                          // TAG_End (never reached)
        };
        Tag t3;
        CHECK(!read(bomb, sizeof(bomb), t3, nullptr, nullptr), "End-typed list bomb should fail");
    }

    // A list claiming far more elements than the remaining bytes could ever supply.
    {
        const uint8_t bomb[] = {
            0x0A, 0x00, 0x00,             // TAG_Compound, name ""
            0x09, 0x00, 0x01, 'x',        // TAG_List "x"
            0x01,                         // element type = TAG_Byte
            0x7F, 0xFF, 0xFF, 0xFF,       // length = 2147483647
            0x00
        };
        Tag t4;
        CHECK(!read(bomb, sizeof(bomb), t4, nullptr, nullptr), "oversized list length should fail");
    }

    // Nesting past the depth cap: 3 bytes per level on the wire, deep enough to overflow
    // the stack if readPayload recursed without a limit.
    {
        std::vector<uint8_t> deep;
        for (int k = 0; k < 20000; ++k) {
            deep.push_back(0x0A);         // TAG_Compound
            deep.push_back(0x00);         // name length hi
            deep.push_back(0x00);         // name length lo
        }
        Tag t5;
        CHECK(!read(deep.data(), deep.size(), t5, nullptr, nullptr), "deep nesting should fail");
    }

    // A mixed-type list cannot be expressed on the wire (one type header covers every
    // element), so write() must refuse it instead of emitting an unreadable file.
    {
        Tag bad = Tag::Compound();
        Tag mixed = Tag::List(Type::Int);
        mixed.items.push_back(Tag::Int(1));
        mixed.items.push_back(Tag::Str("not an int"));
        bad.put("mixed", std::move(mixed));
        std::vector<uint8_t> out;
        CHECK(!write(bad, "root", out), "mixed-type list should not serialize");
    }

    // A failed write must leave the previous file intact, and drop its temp file.
    {
        Tag bad = Tag::Compound();
        Tag mixed = Tag::List(Type::Int);
        mixed.items.push_back(Tag::Int(1));
        mixed.items.push_back(Tag::Str("not an int"));
        bad.put("mixed", std::move(mixed));
        CHECK(!writeFile(path, bad, "root"), "writeFile should fail on invalid tree");
        Tag still;
        CHECK(readFile(path, still) && still.getString("character") == "soldier",
              "failed write must not damage the existing file");
        std::error_code ec;
        CHECK(!std::filesystem::exists(path + ".tmp", ec), "temp file should not survive");
    }

    std::printf("\n%s\n", dump(back, "animation_descriptor").c_str());

    if (failures == 0)
        std::printf("ALL NBT TESTS PASSED\n");
    else
        std::printf("%d NBT TEST(S) FAILED\n", failures);
    return failures;
}
