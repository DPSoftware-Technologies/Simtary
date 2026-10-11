#pragma once
// Shared by the bridge's translation units only (exceptions ON, taipan.h visible).
// Never included from framework code.

#include "StTaipan.h"

#include "taipan.h"

#include <string>
#include <unordered_map>

namespace st::taipan {

struct VM {
    TaipanState* state = nullptr;
    std::unordered_map<std::string, TaipanClassBuilder> classes;
    std::string lastError;
    unsigned chunkCounter = 0; // RunString: every chunk gets a unique name
    std::string hitObjectClass = "GameObject"; // what RaycastHit.gameObject is
    std::string scratch;                       // GetGlobal's string result

    TextFn printFn = nullptr;
    void*  printUser = nullptr;
    TextFn errorFn = nullptr;
    void*  errorUser = nullptr;
    ReadFn readFn = nullptr;
    void*  readUser = nullptr;
};

// The value types as Taipan sees them. Constructible (CtorCall needs T(args...)),
// otherwise the same floats as Vec2 / Vec3 / Quat / Rgba in StTaipan.h.
struct V2 {
    float x = 0, y = 0;
    V2 () = default;
    V2 (float ax, float ay) : x(ax), y(ay) {}
};
struct V3 {
    float x = 0, y = 0, z = 0;
    V3 () = default;
    V3 (float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};
struct Q4 {
    float x = 0, y = 0, z = 0, w = 1;
    Q4 () = default;
    Q4 (float ax, float ay, float az, float aw) : x(ax), y(ay), z(az), w(aw) {}
};
struct C4 {
    float r = 1, g = 1, b = 1, a = 1;
    C4 () = default;
    C4 (float ar, float ag, float ab, float aa) : r(ar), g(ag), b(ab), a(aa) {}
};
struct Hit {
    V3 point, normal;
    float distance = 0;
    void* object = nullptr;   // handle of the object hit
    std::string objectClass;  // its class
};

// Value (StTaipan.h) -> TaipanValue, value types included.
TaipanValue ToTaipan (const Value& v);

} // namespace st::taipan
