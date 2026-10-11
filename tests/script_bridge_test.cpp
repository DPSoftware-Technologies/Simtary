// script_bridge_test - the Taipan bridge (libs/TaipanBridge) seen from a host that
// builds like a game: exceptions OFF, RTTI OFF, StTaipan.h only.
//
// What it pins down is the boundary itself. Every script error below is raised by
// Taipan as a C++ exception; if one ever escaped the bridge into this translation unit
// the process would terminate instead of printing PASS.
//
//	- natives, a class with a property and a method, an engine handle that is not a
//	  real pointer (entity ids are), a nullable return
//	- a native that Fail()s and a native handed the wrong argument type: both must come
//	  back as catchable script errors, not crashes
//	- the file-reader hook: the script and the module it imports exist only in memory,
//	  the way a .tp inside an asset package does
//	- script instances: two environments over one file, each with its own globals
//	- InvalidateObject: a stale handle is a script error, not a dangling read
#include "StTaipan.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace tb = st::taipan;

namespace {

int failures = 0;
std::string printed;
std::string errors;
std::string reported; // last value a script handed to world.report()

void Check (bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// A stand-in for the scene: things addressed by id, the way wi::ecs::Entity is.
struct Thing {
    float x = 0.0f;
    bool alive = false;
};
Thing things[4];

void* HandleOf (uint32_t id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

Thing* Resolve (tb::Call* call, void* self) {
    uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (id == 0 || id >= 4 || !things[id].alive) {
        tb::Fail(call, "thing is gone");
        return nullptr;
    }
    return &things[id];
}

const char* kMover = R"(import util.mathx
int calls = 0
Thing? kept = null
func onStart(Thing self) {
    kept = self
    self.x = util.mathx.twice(1.5)
}
func onUpdate(Thing self, float dt) {
    calls += 1
    self.add(dt)
}
func getCalls() {
    world.report(f"{calls}")
}
func findMissing() {
    Thing? t = world.find("nobody")
    if t == null {
        world.report("null")
    } else {
        world.report("found")
    }
}
func failing() {
    try {
        world.explode()
    } catch err {
        world.report("caught: " + err)
    }
}
func badArg() {
    try {
        world.takesInt(world.anything())
    } catch err {
        world.report("caught")
    }
}
func touchKept() {
    if kept != null {
        kept.add(1.0)
    }
}
)";

const char* kMathx = "func twice(float v) -> float { return v * 2.0 }\n";

// Inspector fields: listed in source order, `_hidden` and the const left out.
const char* kFields = R"(float speed = 4.0
int _hidden = 1
const float K = 2.0
str label = "hi"
Vector3 offset = Vector3(1, 2, 3)
Color tint = Color.red
bool on = true
func report() {
    world.report(f"{speed} {label} {offset}")
}
)";

bool Read (const char* path, tb::Sink* sink, void*) {
    // Paths arrive absolute; match on the tail like a package lookup on the logical path.
    std::string p(path);
    for (char& c : p) if (c == '\\') c = '/';
    auto ends = [&](const char* tail) {
        size_t n = std::strlen(tail);
        return p.size() >= n && p.compare(p.size() - n, n, tail) == 0;
    };
    const char* text = nullptr;
    if (ends("/pkg/mover.tp")) text = kMover;
    else if (ends("/pkg/util/mathx.tp")) text = kMathx;
    else if (ends("/pkg/fields.tp")) text = kFields;
    if (!text) return false;
    tb::SinkWrite(sink, text, std::strlen(text));
    return true;
}

} // namespace

int main () {
    tb::VM* vm = tb::Create();
    Check(vm != nullptr, "VM created");
    if (!vm) return 1;

    tb::SetPrintHandler(vm, [](const char* t, void*) { printed += t; }, nullptr);
    tb::SetErrorHandler(vm, [](const char* t, void*) { errors += t; errors += "\n"; }, nullptr);
    tb::SetFileReader(vm, &Read, nullptr);
    tb::SetScriptRoot(vm, "pkg");  // not on disk: imports must resolve through Read()
    tb::SetBytecodeCache(vm, false);
    tb::SetWarnings(vm, false);

    tb::DefineClass(vm, "Thing", nullptr);
    tb::RegisterProperty(vm, "Thing", "x", "float",
        [](tb::Call* c, void* self, void*) {
            if (Thing* t = Resolve(c, self)) tb::ReturnFloat(c, t->x);
        },
        [](tb::Call* c, void* self, void*) {
            if (Thing* t = Resolve(c, self)) t->x = (float)tb::ArgFloat(c, 0);
        }, nullptr);
    tb::RegisterMethod(vm, "Thing", "add",
        [](tb::Call* c, void* self, void*) {
            if (Thing* t = Resolve(c, self)) t->x += (float)tb::ArgFloat(c, 0);
        }, nullptr, "(float)");
    tb::RegisterFunction(vm, "world.find",
        [](tb::Call* c, void*) { tb::ReturnObject(c, "Thing", nullptr); }, nullptr, "(str) -> Thing?");
    tb::RegisterFunction(vm, "world.explode",
        [](tb::Call* c, void*) { tb::Fail(c, "boom"); }, nullptr, "()");
    tb::RegisterFunction(vm, "world.anything",
        [](tb::Call* c, void*) { tb::ReturnString(c, "not an int"); }, nullptr, "() -> any");
    tb::RegisterFunction(vm, "world.report",
        [](tb::Call* c, void*) { reported = tb::ArgString(c, 0); }, nullptr, "(str)");
    tb::RegisterFunction(vm, "world.takesInt",
        [](tb::Call* c, void*) { tb::ArgInt(c, 0); }, nullptr, "(any)");

    things[1].alive = true;
    things[2].alive = true;

    tb::Script* a = tb::NewScript(vm, "pkg/mover.tp");
    tb::Script* b = tb::NewScript(vm, "pkg/mover.tp");
    Check(a && b, "script loaded through the file reader (import included)");
    if (!a || !b) {
        std::printf("%s\n", errors.c_str());
        return 1;
    }
    Check(tb::RunScript(vm, a) && tb::RunScript(vm, b), "top-level code ran");

    tb::Value self1 = tb::Value::Object("Thing", HandleOf(1));
    tb::Value self2 = tb::Value::Object("Thing", HandleOf(2));
    Check(tb::CallFunction(vm, a, "onStart", &self1, 1), "onStart(self) on instance A");
    Check(tb::CallFunction(vm, b, "onStart", &self2, 1), "onStart(self) on instance B");
    Check(things[1].x == 3.0f, "property setter + imported module");

    tb::Value args[2] = { self1, tb::Value::Float(0.5) };
    for (int i = 0; i < 3; ++i) tb::CallFunction(vm, a, "onUpdate", args, 2);
    Check(things[1].x == 4.5f, "method call through a fake (id) handle");
    Check(things[2].x == 3.0f, "instance B untouched by A's updates");

    // Per-instance globals: A ran update 3 times, B never did.
    Check(tb::HasFunction(vm, a, "getCalls"), "HasFunction");
    tb::CallFunction(vm, a, "getCalls", nullptr, 0);
    Check(reported == "3", "instance A counted its own 3 updates");
    tb::CallFunction(vm, b, "getCalls", nullptr, 0);
    Check(reported == "0", "instance B has its own globals");

    tb::CallFunction(vm, a, "findMissing", nullptr, 0);
    Check(reported == "null", "nullable native return");
    tb::CallFunction(vm, a, "failing", nullptr, 0);
    Check(reported.find("caught") == 0 && reported.find("boom") != std::string::npos,
          "Fail() surfaces as a catchable script error");
    reported.clear();
    tb::CallFunction(vm, a, "badArg", nullptr, 0);
    Check(reported == "caught", "wrong argument type is a catchable script error");

    // An UNcaught error: must come back as false, with the diagnostics in the handler.
    errors.clear();
    things[1].alive = false;
    bool ok = tb::CallFunction(vm, a, "onUpdate", args, 2);
    Check(!ok && errors.find("thing is gone") != std::string::npos, "uncaught native error -> false + diagnostics");

    // Invalidated handle: B kept its `self` in a global. Once the engine says the object
    //	is gone, using that stored handle must raise, even though Resolve() would still
    //	find things[2] alive - the VM refuses before the native runs.
    Check(tb::CallFunction(vm, b, "touchKept", nullptr, 0) && things[2].x == 4.0f, "kept handle works");
    tb::InvalidateObject(vm, HandleOf(2));
    errors.clear();
    ok = tb::CallFunction(vm, b, "touchKept", nullptr, 0);
    Check(!ok, "invalidated handle is a script error, not a crash");

    // A syntax error must not take the process down either.
    errors.clear();
    Check(!tb::RunString(vm, "func (", "broken"), "syntax error -> false");
    Check(!errors.empty(), "syntax error reported to the handler");

    Check(tb::RunString(vm, "print(\"hello from the console\")", "console"), "RunString");
    Check(printed.find("hello from the console") != std::string::npos, "print handler");
    // The console reuses one chunk name; each run must execute ITS code, not the first.
    Check(tb::RunString(vm, "print(\"second chunk\")", "console"), "RunString again, same chunk name");
    Check(printed.find("second chunk") != std::string::npos, "second console chunk ran its own code");

    // ---- Unity math (0.2.0 value types) across the boundary ----
    tb::RegisterUnityMath(vm, "Thing");
    tb::SetStatic(vm, "Time", "deltaTime", tb::Value::Float(0.5));
    tb::RegisterFunction(vm, "world.echo",   // a Vector3 in, a Vector3 out, through POD
        [](tb::Call* c, void*) {
            tb::Vec3 v = tb::ArgVector3(c, 0);
            tb::ReturnVector3(c, tb::Vec3{ v.x * 2, v.y * 2, v.z * 2 });
        }, nullptr, "(Vector3) -> Vector3");
    tb::RegisterFunction(vm, "world.hit",
        [](tb::Call* c, void*) {
            tb::Return(c, tb::Value::RaycastHit(tb::Vec3{ 1, 2, 3 }, tb::Vec3{ 0, 1, 0 }, 4.5f, HandleOf(1)));
        }, nullptr, "() -> RaycastHit?");
    printed.clear();
    errors.clear();
    const bool mathOk = tb::RunString(vm, R"(
Vector3 p = Vector3(1, 2, 3)
p += Vector3.up * 2.0 * Time.deltaTime
print(f"{p}")
print(f"{world.echo(p)}")
print(f"{Vector3.Distance(Vector3.zero, Vector3(3, 4, 0))}")
Quaternion q = Quaternion.Euler(0, 90, 0)
Vector3 f = q * Vector3.forward
print(f"{math.round(f.x * 1000.0)} {math.round(f.z * 1000.0)}")
Vector3 e = Quaternion.Euler(10, 20, 30).eulerAngles
print(f"{math.round(e.x)} {math.round(e.y)} {math.round(e.z)}")
print(f"{Mathf.Clamp(5.0, 0.0, 1.0)} {Color.red == Color(1, 0, 0, 1)}")
RaycastHit? h = world.hit()
if h != null {
    print(f"{h.distance} {h.point}")
}
)", "math");
    if (!mathOk) std::printf("%s\n", errors.c_str());
    Check(mathOk, "Unity math script ran");
    Check(printed.find("(1.00, 3.00, 3.00)") != std::string::npos, "Vector3 operators + Time.deltaTime static");
    Check(printed.find("(2.00, 6.00, 6.00)") != std::string::npos, "Vector3 passed through a native (POD)");
    Check(printed.find("5.0") != std::string::npos, "Vector3.Distance");
    Check(printed.find("1000 0") != std::string::npos, "Quaternion.Euler(0,90,0) * forward = right");
    Check(printed.find("10 20 30") != std::string::npos, "Euler -> quaternion -> eulerAngles round-trips");
    Check(printed.find("1.0 true") != std::string::npos, "Mathf.Clamp, Color ==");
    Check(printed.find("4.5 (1.00, 2.00, 3.00)") != std::string::npos, "RaycastHit value from the host");
    if (failures) std::printf("--- printed ---\n%s\n", printed.c_str());

    // ---- inspector fields ----
    tb::Script* fs = tb::NewScript(vm, "pkg/fields.tp");
    Check(fs && tb::RunScript(vm, fs), "fields script loaded");
    std::string listed;
    tb::ListFields(vm, fs, [](const char* name, tb::Type, void* user) {
        *static_cast<std::string*>(user) += std::string(name) + ",";
    }, &listed);
    Check(listed == "speed,label,offset,tint,on,", "ListFields: source order, _hidden and const skipped");
    tb::Value v;
    Check(tb::GetGlobal(vm, fs, "speed", &v) && v.type == tb::Type::Float && v.f == 4.0, "GetGlobal float");
    Check(tb::GetGlobal(vm, fs, "offset", &v) && v.type == tb::Type::Vector3 && v.v[2] == 3.0f, "GetGlobal Vector3");
    tb::SetGlobal(vm, fs, "speed", tb::Value::Float(9.5));
    tb::SetGlobal(vm, fs, "label", tb::Value::String("tuned"));
    tb::SetGlobal(vm, fs, "offset", tb::Value::Vector3(tb::Vec3{ 7, 8, 9 }));
    reported.clear();
    tb::CallFunction(vm, fs, "report", nullptr, 0);
    Check(reported == "9.5 tuned (7.00, 8.00, 9.00)", "SetGlobal reaches the running script");
    if (reported != "9.5 tuned (7.00, 8.00, 9.00)") std::printf("reported: %s\n%s\n", reported.c_str(), errors.c_str());
    tb::FreeScript(vm, fs);

    tb::FreeScript(vm, a);
    tb::FreeScript(vm, b);
    tb::Destroy(vm);

    std::printf(failures ? "\n%d FAILED\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
