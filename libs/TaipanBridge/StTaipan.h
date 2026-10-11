#pragma once
// st::taipan - the exception-free face of the Taipan VM (libs/Taipan).
//
// Taipan throws a C++ exception for every script error and hands std::string /
// std::function across its own header. Games here build with /EHsc- /GR-
// _HAS_EXCEPTIONS=0, so that header is off limits to engine and framework code - the
// same situation as JSBSim. This file is the whole surface they get instead:
//
//	- no exceptions cross it. Every entry point catches on the Taipan side and reports
//	  failure by return value plus the error handler.
//	- no standard library type crosses it: text is const char*, callbacks are plain
//	  function pointers with a void* user slot.
//	- a native written against it never throws either. A bad argument or a
//	  Fail() records the error on the Call, and the bridge raises the script
//	  error AFTER the native has returned, so the unwind only ever runs through
//	  frames that were compiled with exceptions on.
//
// One VM is single-threaded. Call everything for a given VM from one thread (the
// framework uses the main thread - see Framework/script/ScriptSystem.h).

#include <cstddef>
#include <cstdint>

namespace st::taipan {

struct VM;     // one Taipan state
struct Script; // one script instance: own globals over shared bytecode
struct Call;   // one native call in flight

enum class Type : uint8_t {
    Null,
    Bool,
    Int,
    Float,
    String,
    Object,     // an engine-owned handle (see DefineClass)
    // value types (RegisterUnityMath): immutable copies, owned by the script
    Vector2,
    Vector3,
    Quaternion,
    Color,
    RaycastHit,
    Other,      // list, dict, function, script object - not readable through the bridge
};

// The value types as plain data. Layout-compatible with XMFLOAT2/3/4, so engine code
//	can copy them straight across.
struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Rgba { float r = 1, g = 1, b = 1, a = 1; };

// A value going INTO the VM (call arguments, return values). Strings are copied by the
// VM on the way in, so they only need to live until the call that takes them returns.
struct Value {
    Type        type = Type::Null;
    bool        b = false;
    int64_t     i = 0;
    double      f = 0.0;
    const char* str = nullptr;   // String: the text (NUL-terminated). Object: the class name.
    void*       object = nullptr; // Object: the handle. RaycastHit: the hit object's handle.
    float       v[7] = {};        // value types: x y z w / r g b a / point normal distance

    static Value Null () { return Value{}; }
    static Value Bool (bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value Int (int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value Float (double v) { Value x; x.type = Type::Float; x.f = v; return x; }
    static Value String (const char* v) { Value x; x.type = Type::String; x.str = v; return x; }
    // A null `ptr` becomes a script null, which is what a nullable return wants.
    static Value Object (const char* className, void* ptr) {
        Value x; x.type = ptr ? Type::Object : Type::Null; x.str = className; x.object = ptr; return x;
    }
    static Value Vector2 (Vec2 a) { Value x; x.type = Type::Vector2; x.v[0] = a.x; x.v[1] = a.y; return x; }
    static Value Vector3 (Vec3 a) {
        Value x; x.type = Type::Vector3; x.v[0] = a.x; x.v[1] = a.y; x.v[2] = a.z; return x;
    }
    static Value Quaternion (Quat a) {
        Value x; x.type = Type::Quaternion; x.v[0] = a.x; x.v[1] = a.y; x.v[2] = a.z; x.v[3] = a.w; return x;
    }
    static Value Color (Rgba a) {
        Value x; x.type = Type::Color; x.v[0] = a.r; x.v[1] = a.g; x.v[2] = a.b; x.v[3] = a.a; return x;
    }
    // `objectHandle` is what hit.gameObject returns (an Object of the class given to
    //	RegisterUnityMath). Null for "nothing hit" is the caller's job: return Null().
    static Value RaycastHit (Vec3 point, Vec3 normal, float distance, void* objectHandle) {
        Value x; x.type = Type::RaycastHit;
        x.v[0] = point.x; x.v[1] = point.y; x.v[2] = point.z;
        x.v[3] = normal.x; x.v[4] = normal.y; x.v[5] = normal.z; x.v[6] = distance;
        x.object = objectHandle;
        return x;
    }
};

using TextFn   = void (*)(const char* text, void* user);
using NativeFn = void (*)(Call* call, void* user);
using MethodFn = void (*)(Call* call, void* self, void* user);
// A getter answers through Return*(). A setter reads the new value as argument 0.
using GetterFn = void (*)(Call* call, void* self, void* user);
using SetterFn = void (*)(Call* call, void* self, void* user);
// Source reader: fill `sink` with SinkWrite() and return true, or return false to let
//	the VM try the disk. `path` is absolute.
struct Sink;
using ReadFn = bool (*)(const char* path, Sink* sink, void* user);
void SinkWrite (Sink* sink, const void* data, size_t size);

// ---------------------------------------------------------------------------
// VM lifecycle and configuration
// ---------------------------------------------------------------------------
VM*  Create ();
void Destroy (VM* vm);

void SetPrintHandler (VM* vm, TextFn fn, void* user);  // script print()
void SetErrorHandler (VM* vm, TextFn fn, void* user);  // formatted diagnostics
void SetFileReader (VM* vm, ReadFn fn, void* user);
void SetScriptRoot (VM* vm, const char* directory);    // where `import a.b` resolves
void SetBytecodeCache (VM* vm, bool enabled);
void SetWarnings (VM* vm, bool enabled);

// The last diagnostics text (empty when the last operation succeeded). Valid until the
//	next call into this VM.
const char* LastError (VM* vm);

// ---------------------------------------------------------------------------
// Bindings. Register BEFORE loading the scripts that use them: the type checker reads
// the signatures at load time. Signature format: "(int, str=, float...) -> Entity?".
// ---------------------------------------------------------------------------
bool RegisterFunction (VM* vm, const char* dottedName, NativeFn fn, void* user, const char* signature);
// Declares a native class. `alias` (may be null) is an extra type name scripts may use.
//	(Not "RegisterClass": <windows.h> macros that to RegisterClassW in any TU that sees it.)
bool DefineClass (VM* vm, const char* name, const char* alias);
bool RegisterMethod (VM* vm, const char* className, const char* name, MethodFn fn, void* user,
                     const char* signature);
// `setter` null makes the property read-only. `type` is the script type ("float", ...).
bool RegisterProperty (VM* vm, const char* className, const char* name, const char* type,
                       GetterFn getter, SetterFn setter, void* user);
// The engine object behind `ptr` is gone: script handles to it become stale (a
//	catchable script error on use, never a dangling read).
void InvalidateObject (VM* vm, void* ptr);
// Write every registered binding as a .tpi stub, for the language server and CI.
bool DumpBindings (VM* vm, const char* path);

// The Unity math vocabulary, implemented here (pure math, no engine needed):
//	Vector2, Vector3, Quaternion, Color   value types: constructors, operators, statics
//	                                       (Vector3.zero, Vector3.Distance, Quaternion.Euler...)
//	RaycastHit                             point, normal, distance, gameObject
//	Mathf                                  Clamp, Lerp, PI, Deg2Rad, SmoothStep, ...
//	Space, ForceMode                       Space.Self / Space.World, ForceMode.Impulse, ...
// `objectClass` is the class hit.gameObject is typed as ("GameObject").
void RegisterUnityMath (VM* vm, const char* objectClass);

// The same math, for the host: the engine side of Transform.rotation / eulerAngles /
//	LookAt has to agree with what scripts compute, so it calls these rather than its own.
Quat EulerToQuaternion (Vec3 degrees);           // Unity order: Z, then X, then Y
Vec3 QuaternionToEuler (Quat q);                 // degrees, each in [0, 360)
Quat LookRotation (Vec3 forward, Vec3 up);
Quat QuaternionMultiply (Quat a, Quat b);        // a * b: rotate by b, then by a
Vec3 RotateVector (Quat q, Vec3 v);

// A static that changes at runtime - `Time.deltaTime`, `Input.mousePosition`. Scripts
//	read it as a constant; the host re-sets it (every frame, before scripts run). The
//	type is taken from the value, so set it once before scripts load and keep the kind.
bool SetStatic (VM* vm, const char* className, const char* name, const Value& value);

// Everything a script can call, for editor completion: the standard library AND the
//	host's bindings. `owner` is the namespace ("math", "" for globals) or the class name;
//	`detail` is the signature ("(float) -> float"), a property's type, or for a class
//	its aliases ("entity,ent").
enum class SymbolKind : uint8_t { Function, Value, Class, Method, Property };
using SymbolFn = void (*)(SymbolKind kind, const char* owner, const char* name, const char* detail, void* user);
void ListSymbols (VM* vm, SymbolFn fn, void* user);

// ---------------------------------------------------------------------------
// Running code
// ---------------------------------------------------------------------------
// Compile and run a chunk of source in the VM's main environment (the console).
bool RunString (VM* vm, const char* source, const char* chunkName);
// Advance script time by dt seconds and resume due coroutines.
void Update (VM* vm, double dt);
int  CoroutineCount (VM* vm);
// Hot reload: every live instance of `path` gets the new functions and keeps its state.
bool Reload (VM* vm, const char* path);
// Parse and type-check `path` (and its imports) without running anything. False with
//	the diagnostics in LastError() / the error handler when it does not compile.
bool Check (VM* vm, const char* path);

// Instances. NewScript returns null on a compile error (reported to the error handler).
Script* NewScript (VM* vm, const char* path);
bool    RunScript (VM* vm, Script* script);  // its top-level code
bool    HasFunction (VM* vm, Script* script, const char* name);
// Calls `name(args...)`. False when the call raised; the error went to the handler.
bool    CallFunction (VM* vm, Script* script, const char* name, const Value* args, int argc);
void    FreeScript (VM* vm, Script* script); // also stops its coroutines

// A script's inspector fields - Unity's public fields: its top-level variables of type
//	int, float, bool, str, Vector2, Vector3, Quaternion or Color, in source order. A
//	`const`, or a name starting with '_', stays hidden.
using FieldFn = void (*)(const char* name, Type type, void* user);
void ListFields (VM* vm, Script* script, FieldFn fn, void* user);
// Read / write one of a script's globals. GetGlobal's string stays valid until the next
//	call into this VM. False when the name is not a global of a supported type.
bool GetGlobal (VM* vm, Script* script, const char* name, Value* out);
bool SetGlobal (VM* vm, Script* script, const char* name, const Value& value);

// ---------------------------------------------------------------------------
// Inside a native. Readers never throw: a type mismatch records an error on the call
// and returns a zero value; the script sees the error once the native returns.
// ---------------------------------------------------------------------------
int         ArgCount (const Call* call);
Type        ArgType (const Call* call, int index);           // Type::Null past the end
int64_t     ArgInt (Call* call, int index);                  // floats truncate
double      ArgFloat (Call* call, int index);                // ints widen
bool        ArgBool (Call* call, int index);
const char* ArgString (Call* call, int index);               // valid until the native returns
void*       ArgObject (Call* call, int index, const char* className); // null for a null arg
// Any argument as text, the way print() shows it: "(1.00, 2.00, 3.00)", "true", "hp".
const char* ArgText (Call* call, int index);
Vec2        ArgVector2 (Call* call, int index);
Vec3        ArgVector3 (Call* call, int index);
Quat        ArgQuaternion (Call* call, int index);
Rgba        ArgColor (Call* call, int index);

void Return (Call* call, const Value& value);
inline void ReturnBool (Call* call, bool v) { Return(call, Value::Bool(v)); }
inline void ReturnInt (Call* call, int64_t v) { Return(call, Value::Int(v)); }
inline void ReturnFloat (Call* call, double v) { Return(call, Value::Float(v)); }
inline void ReturnString (Call* call, const char* v) { Return(call, Value::String(v)); }
inline void ReturnObject (Call* call, const char* className, void* ptr) {
    Return(call, Value::Object(className, ptr));
}
inline void ReturnVector2 (Call* call, Vec2 v) { Return(call, Value::Vector2(v)); }
inline void ReturnVector3 (Call* call, Vec3 v) { Return(call, Value::Vector3(v)); }
inline void ReturnQuaternion (Call* call, Quat v) { Return(call, Value::Quaternion(v)); }
inline void ReturnColor (Call* call, Rgba v) { Return(call, Value::Color(v)); }

// Raise a catchable script error with this message once the native returns.
void Fail (Call* call, const char* message);
bool Failed (const Call* call);

} // namespace st::taipan
