// StTaipan.cpp - the only translation unit outside libs/Taipan that includes taipan.h.
//
// Built inside SimtaryScript with exceptions ON (see CMakeLists.txt). Every function
// here is a catch boundary: nothing thrown by Taipan, or by the standard library on
// Taipan's side, is allowed to reach a caller compiled with /EHsc-.
#include "StTaipanInternal.h"

#include "internal.h" // HostError: what Taipan throws for a bad native argument
#include "loader.h"   // Loader::Entry::syms: a script's declared globals, for ListFields

#include <algorithm>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::taipan {

struct Script {
    TaipanScript* handle = nullptr;
    std::string path; // absolute and normalized, the way the loader keys its entries
};

struct Sink {
    std::string* out = nullptr;
};

struct Call {
    const TaipanArgs*  args = nullptr;        // function / method / getter arguments
    const TaipanValue* setterValue = nullptr; // property write: the value IS argument 0
    TaipanValue result;
    std::string error;
    bool failed = false;
    std::deque<std::string> strings; // backing store for ArgString (stable addresses)

    explicit Call (const TaipanArgs& a) : args(&a) {}
    explicit Call (const TaipanValue& assigned) : setterValue(&assigned) {}

    void RecordError (std::string message) {
        if (failed) return; // the first error is the one worth reading
        failed = true;
        error = std::move(message);
    }
};

TaipanValue ToTaipan (const Value& v) {
    switch (v.type) {
    case Type::Bool:       return TaipanValue::fromBool(v.b);
    case Type::Int:        return TaipanValue::fromInt(v.i);
    case Type::Float:      return TaipanValue::fromFloat(v.f);
    case Type::String:     return TaipanValue::fromString(v.str ? v.str : "");
    case Type::Object:     return TaipanValue::fromObject(v.str, v.object);
    case Type::Vector2:    return TaipanValue::fromValue(V2(v.v[0], v.v[1]));
    case Type::Vector3:    return TaipanValue::fromValue(V3(v.v[0], v.v[1], v.v[2]));
    case Type::Quaternion: return TaipanValue::fromValue(Q4(v.v[0], v.v[1], v.v[2], v.v[3]));
    case Type::Color:      return TaipanValue::fromValue(C4(v.v[0], v.v[1], v.v[2], v.v[3]));
    case Type::RaycastHit: {
        Hit h;
        h.point = V3(v.v[0], v.v[1], v.v[2]);
        h.normal = V3(v.v[3], v.v[4], v.v[5]);
        h.distance = v.v[6];
        h.object = v.object;
        h.objectClass = v.str ? v.str : "";
        return TaipanValue::fromValue(h);
    }
    default:               return TaipanValue::null();
    }
}

namespace {

// Which of the bridge's value types a boxed value is, by its class name.
Type ValueTypeOf (const std::string& className) {
    if (className == "Vector2")    return Type::Vector2;
    if (className == "Vector3")    return Type::Vector3;
    if (className == "Quaternion") return Type::Quaternion;
    if (className == "Color")      return Type::Color;
    if (className == "RaycastHit") return Type::RaycastHit;
    return Type::Other;
}

Type FromTaipanType (TaipanValue::Type t) {
    switch (t) {
    case TaipanValue::Type::Null:   return Type::Null;
    case TaipanValue::Type::Bool:   return Type::Bool;
    case TaipanValue::Type::Int:    return Type::Int;
    case TaipanValue::Type::Float:  return Type::Float;
    case TaipanValue::Type::String: return Type::String;
    case TaipanValue::Type::Object: return Type::Object;
    default:                        return Type::Other;
    }
}

std::string ArgLabel (int index) {
    return "argument " + std::to_string(index + 1);
}

// Run `body`, converting anything thrown into a false return plus lastError. The
//	Taipan API mostly reports by return value already; this is for what it does not
//	catch itself (allocation failure, a std::filesystem error, a bridge bug).
template <class F>
bool Guard (VM* vm, F&& body) {
    try {
        return body();
    } catch (std::exception& e) {
        vm->lastError = e.what();
    } catch (...) {
        vm->lastError = "unknown error inside the Taipan VM";
    }
    if (vm->errorFn) vm->errorFn(vm->lastError.c_str(), vm->errorUser);
    return false;
}

// The native wrappers. Each runs the EH-off callback to completion, THEN raises the
//	recorded error - so the throw starts here, on the exceptions-on side.
TaipanValue Finish (Call& call, const TaipanArgs& args) {
    if (call.failed) args.error(call.error);
    return call.result;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
VM* Create () {
    try {
        VM* vm = new VM();
        vm->state = taipan_create();
        // Diagnostics land in lastError and the host's handler, never on stderr.
        taipan_set_error_handler(vm->state, [vm](const std::string& text) {
            if (vm->errorFn) vm->errorFn(text.c_str(), vm->errorUser);
        });
        taipan_set_print_handler(vm->state, [vm](const std::string& text) {
            if (vm->printFn) vm->printFn(text.c_str(), vm->printUser);
        });
        return vm;
    } catch (...) {
        return nullptr;
    }
}

void Destroy (VM* vm) {
    if (!vm) return;
    try {
        vm->classes.clear();
        taipan_destroy(vm->state);
    } catch (...) {
    }
    delete vm;
}

void SetPrintHandler (VM* vm, TextFn fn, void* user) {
    vm->printFn = fn;
    vm->printUser = user;
}

void SetErrorHandler (VM* vm, TextFn fn, void* user) {
    vm->errorFn = fn;
    vm->errorUser = user;
}

void SinkWrite (Sink* sink, const void* data, size_t size) {
    try {
        sink->out->assign(static_cast<const char*>(data), size);
    } catch (...) {
    }
}

void SetFileReader (VM* vm, ReadFn fn, void* user) {
    vm->readFn = fn;
    vm->readUser = user;
    Guard(vm, [&] {
        if (!fn) {
            taipan_set_file_reader(vm->state, nullptr);
            return true;
        }
        taipan_set_file_reader(vm->state, [vm](const std::string& path, std::string& out) {
            Sink sink{ &out };
            return vm->readFn(path.c_str(), &sink, vm->readUser);
        });
        return true;
    });
}

void SetScriptRoot (VM* vm, const char* directory) {
    Guard(vm, [&] { taipan_set_script_root(vm->state, directory); return true; });
}

void SetBytecodeCache (VM* vm, bool enabled) {
    taipan_set_bytecode_cache(vm->state, enabled, nullptr);
}

void SetWarnings (VM* vm, bool enabled) {
    taipan_set_warnings(vm->state, enabled);
}

const char* LastError (VM* vm) {
    return vm->lastError.c_str();
}

// ---------------------------------------------------------------------------
// bindings
// ---------------------------------------------------------------------------
bool RegisterFunction (VM* vm, const char* dottedName, NativeFn fn, void* user, const char* signature) {
    return Guard(vm, [&] {
        taipan_register_function(vm->state, dottedName, [fn, user](TaipanArgs args) {
            Call call(args);
            fn(&call, user);
            return Finish(call, args);
        }, signature);
        return true;
    });
}

bool DefineClass (VM* vm, const char* name, const char* alias) {
    return Guard(vm, [&] {
        auto it = vm->classes.find(name);
        if (it == vm->classes.end())
            it = vm->classes.emplace(name, taipan_register_class(vm->state, name)).first;
        if (alias) it->second.alias(alias);
        return true;
    });
}

bool RegisterMethod (VM* vm, const char* className, const char* name, MethodFn fn, void* user,
                     const char* signature) {
    return Guard(vm, [&] {
        auto it = vm->classes.find(className);
        if (it == vm->classes.end()) {
            vm->lastError = std::string("RegisterMethod: class '") + className + "' is not registered";
            return false;
        }
        it->second.method(name, [fn, user](void* self, TaipanArgs args) {
            Call call(args);
            fn(&call, self, user);
            return Finish(call, args);
        }, signature);
        return true;
    });
}

bool RegisterProperty (VM* vm, const char* className, const char* name, const char* type,
                       GetterFn getter, SetterFn setter, void* user) {
    return Guard(vm, [&] {
        auto it = vm->classes.find(className);
        if (it == vm->classes.end()) {
            vm->lastError = std::string("RegisterProperty: class '") + className + "' is not registered";
            return false;
        }
        // Taipan hands a getter no arguments and a setter one boxed value instead of a
        //	TaipanArgs. The getter runs against an empty list; the setter's Call carries the
        //	value, and the readers below serve it as argument 0.
        TaipanGetterFn get = [getter, user](void* self) {
            TaipanArgs none;
            Call call(none);
            getter(&call, self, user);
            if (call.failed) taipan_detail::fail(call.error);
            return call.result;
        };
        TaipanSetterFn set;
        if (setter) {
            set = [setter, user](void* self, const TaipanValue& value) {
                Call call(value);
                setter(&call, self, user);
                if (call.failed) taipan_detail::fail(call.error);
            };
        }
        it->second.property(name, get, set, type);
        return true;
    });
}

bool SetStatic (VM* vm, const char* className, const char* name, const Value& value) {
    return Guard(vm, [&] {
        auto it = vm->classes.find(className);
        if (it == vm->classes.end())
            it = vm->classes.emplace(className, taipan_register_class(vm->state, className)).first;
        TaipanClassBuilder& c = it->second;
        // The script type follows the C++ type handed to constant<>(), so keep each kind.
        switch (value.type) {
        case Type::Bool:       c.constant(name, value.b); break;
        case Type::Int:        c.constant(name, value.i); break;
        case Type::Float:      c.constant(name, value.f); break;
        case Type::String:     c.constant(name, std::string(value.str ? value.str : "")); break;
        case Type::Vector2:    c.constant(name, V2(value.v[0], value.v[1])); break;
        case Type::Vector3:    c.constant(name, V3(value.v[0], value.v[1], value.v[2])); break;
        case Type::Quaternion: c.constant(name, Q4(value.v[0], value.v[1], value.v[2], value.v[3])); break;
        case Type::Color:      c.constant(name, C4(value.v[0], value.v[1], value.v[2], value.v[3])); break;
        default:
            vm->lastError = std::string("SetStatic: unsupported value for ") + className + "." + name;
            return false;
        }
        return true;
    });
}

void InvalidateObject (VM* vm, void* ptr) {
    Guard(vm, [&] { taipan_invalidate_object(vm->state, ptr); return true; });
}

bool DumpBindings (VM* vm, const char* path) {
    return Guard(vm, [&] { return taipan_dump_bindings(vm->state, path); });
}

void ListSymbols (VM* vm, SymbolFn fn, void* user) {
    // Straight from the VM's tables (internal.h): taipan_dump_bindings only covers what
    //	the HOST registered, and completion wants math.sin as much as scene.find.
    Guard(vm, [&] {
        tp::VM& v = vm->state->vm;
        for (const auto& kv : v.nativeSigs) {
            const std::string& q = kv.first;
            const size_t dot = q.rfind('.');
            const std::string owner = dot == std::string::npos ? std::string() : q.substr(0, dot);
            const std::string name = dot == std::string::npos ? q : q.substr(dot + 1);
            // "(float) -> float" is a function; a bare type ("float") is a constant.
            const SymbolKind kind = (!kv.second.empty() && kv.second[0] == '(') ? SymbolKind::Function : SymbolKind::Value;
            fn(kind, owner.c_str(), name.c_str(), kv.second.c_str(), user);
        }
        for (const auto& kv : v.nativeClasses) {
            const tp::NativeClass& c = *kv.second;
            std::string aliases; // "entity,ent": the other names scripts may use
            for (const std::string& a : c.aliases) aliases += (aliases.empty() ? "" : ",") + a;
            fn(SymbolKind::Class, "", c.name.c_str(), aliases.c_str(), user);
            for (const auto& p : c.props)
                fn(SymbolKind::Property, c.name.c_str(), p.first.c_str(), p.second.type.c_str(), user);
            for (const auto& m : c.methods)
                fn(SymbolKind::Method, c.name.c_str(), m.first.c_str(), m.second.sig.c_str(), user);
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// running code
// ---------------------------------------------------------------------------
namespace {

bool Track (VM* vm, bool ok) {
    vm->lastError = ok ? std::string() : taipan_last_error(vm->state);
    return ok;
}

} // namespace

bool RunString (VM* vm, const char* source, const char* chunkName) {
    return Guard(vm, [&] {
        // Taipan keys modules by chunk name and reuses an existing one, so loading new
        //	source under a name it has seen re-runs the OLD code. Number every chunk.
        std::string chunk = std::string(chunkName ? chunkName : "chunk") + "#" + std::to_string(++vm->chunkCounter);
        if (!taipan_load_string(vm->state, source, chunk.c_str())) return Track(vm, false);
        return Track(vm, taipan_run(vm->state));
    });
}

void Update (VM* vm, double dt) {
    Guard(vm, [&] { taipan_update(vm->state, dt); return true; });
}

int CoroutineCount (VM* vm) {
    int n = 0;
    Guard(vm, [&] { n = taipan_coroutine_count(vm->state); return true; });
    return n;
}

bool Reload (VM* vm, const char* path) {
    return Guard(vm, [&] { return Track(vm, taipan_reload_file(vm->state, path)); });
}

bool Check (VM* vm, const char* path) {
    return Guard(vm, [&] { return Track(vm, taipan_check_file(vm->state, path)); });
}

Script* NewScript (VM* vm, const char* path) {
    Script* out = nullptr;
    Guard(vm, [&] {
        TaipanScript* h = taipan_script_new(vm->state, path);
        if (!h) return Track(vm, false);
        std::error_code ec;
        out = new Script{ h, std::filesystem::absolute(std::filesystem::path(path), ec).lexically_normal().string() };
        return Track(vm, true);
    });
    return out;
}

namespace {

// The bridge type a script global's DECLARED type maps to (Other = not an inspector field).
Type FieldTypeOf (const tp::TypeP& t) {
    if (!t || t->nullable) return Type::Other;
    switch (t->k) {
    case tp::TK::Int:    return Type::Int;
    case tp::TK::Float:  return Type::Float;
    case tp::TK::Bool:   return Type::Bool;
    case tp::TK::Str:    return Type::String;
    case tp::TK::Native: return t->native ? ValueTypeOf(t->native->name) : Type::Other;
    default:             return Type::Other;
    }
}

// The compiled symbols behind an instance: instances share their file's loader entry.
const tp::ModuleSymbols* SymbolsOf (VM* vm, const Script* script) {
    for (const auto& kv : vm->state->vm.loader->entries)
        if (kv.second && kv.second->path == script->path && kv.second->syms) return kv.second->syms.get();
    return nullptr;
}

} // namespace

void ListFields (VM* vm, Script* script, FieldFn fn, void* user) {
    if (!script) return;
    Guard(vm, [&] {
        const tp::ModuleSymbols* syms = SymbolsOf(vm, script);
        if (!syms) return true;
        struct Field { int start; std::string name; Type type; };
        std::vector<Field> fields;
        for (const auto& kv : syms->globals) {
            const tp::ModuleSymbols::Global& g = kv.second;
            if (g.kind != tp::ModuleSymbols::Global::Var || g.isConst || g.name.empty() || g.name[0] == '_') continue;
            const Type t = FieldTypeOf(g.type);
            if (t == Type::Other || t == Type::RaycastHit) continue;
            fields.push_back(Field{ g.span.start, g.name, t });
        }
        // Source order, the way Unity lists a component's fields.
        std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) { return a.start < b.start; });
        for (const Field& f : fields) fn(f.name.c_str(), f.type, user);
        return true;
    });
}

bool GetGlobal (VM* vm, Script* script, const char* name, Value* out) {
    if (!script || !out) return false;
    return Guard(vm, [&] {
        const TaipanValue v = taipan_script_get(vm->state, script->handle, name);
        Value r;
        if (v.isBool())        r = Value::Bool(v.asBool());
        else if (v.isInt())    r = Value::Int(v.asInt());
        else if (v.isFloat())  r = Value::Float(v.asFloat());
        else if (v.isString()) {
            vm->scratch = v.asString();
            r = Value::String(vm->scratch.c_str());
        } else if (const V2* p = v.asValue<V2>()) r = Value::Vector2(Vec2{ p->x, p->y });
        else if (const V3* p = v.asValue<V3>())   r = Value::Vector3(Vec3{ p->x, p->y, p->z });
        else if (const Q4* p = v.asValue<Q4>())   r = Value::Quaternion(Quat{ p->x, p->y, p->z, p->w });
        else if (const C4* p = v.asValue<C4>())   r = Value::Color(Rgba{ p->r, p->g, p->b, p->a });
        else return false;
        *out = r;
        return true;
    });
}

bool SetGlobal (VM* vm, Script* script, const char* name, const Value& value) {
    if (!script) return false;
    return Guard(vm, [&] {
        taipan_script_set(vm->state, script->handle, name, ToTaipan(value));
        return true;
    });
}

bool RunScript (VM* vm, Script* script) {
    return Guard(vm, [&] { return Track(vm, taipan_script_run(vm->state, script->handle)); });
}

bool HasFunction (VM* vm, Script* script, const char* name) {
    bool has = false;
    Guard(vm, [&] { has = taipan_script_has_function(vm->state, script->handle, name); return true; });
    return has;
}

bool CallFunction (VM* vm, Script* script, const char* name, const Value* args, int argc) {
    return Guard(vm, [&] {
        std::vector<TaipanValue> values;
        values.reserve(argc);
        for (int i = 0; i < argc; ++i) values.push_back(ToTaipan(args[i]));
        taipan_script_call(vm->state, script->handle, name, values);
        return Track(vm, taipan_call_ok(vm->state));
    });
}

void FreeScript (VM* vm, Script* script) {
    if (!script) return;
    Guard(vm, [&] { taipan_script_free(vm->state, script->handle); return true; });
    delete script;
}

// ---------------------------------------------------------------------------
// inside a native
// ---------------------------------------------------------------------------
namespace {

// A property write has no TaipanArgs, only the assigned value (see RegisterProperty).
//	Every reader below goes through this to serve it as argument 0.
bool IsSetter (const Call* call) {
    return call->setterValue != nullptr;
}

const TaipanValue* SetterValue (const Call* call, int index) {
    return index == 0 ? call->setterValue : nullptr;
}

} // namespace

int ArgCount (const Call* call) {
    return IsSetter(call) ? 1 : call->args->count();
}

Type ArgType (const Call* call, int index) {
    if (IsSetter(call)) {
        const TaipanValue* v = SetterValue(call, index);
        if (!v) return Type::Null;
        if (v->isValue()) return ValueTypeOf(v->objectClass());
        return v->isObject() ? Type::Object : FromTaipanType(v->type());
    }
    if (index < 0 || index >= call->args->count()) return Type::Null;
    TaipanValue::Type t = call->args->typeAt(index);
    if (t == TaipanValue::Type::Value) {
        try {
            return ValueTypeOf((*call->args)[index].objectClass());
        } catch (...) {
            return Type::Other;
        }
    }
    return t == TaipanValue::Type::Object ? Type::Object : FromTaipanType(t);
}

namespace {

// A value-type argument as its bridge struct, or a recorded error and a default.
template <class T>
T ReadValue (Call* call, int index, const char* className) {
    try {
        if (const TaipanValue* v = SetterValue(call, index)) {
            if (const T* p = v->asValue<T>()) return *p;
        } else if (!IsSetter(call) && index >= 0 && index < call->args->count()) {
            return *static_cast<const T*>(call->args->getValue(index, className));
        }
    } catch (tp::HostError& e) {
        call->RecordError(e.message);
        return T();
    } catch (...) {
    }
    call->RecordError(ArgLabel(index) + ": expected '" + className + "'");
    return T();
}

} // namespace

const char* ArgText (Call* call, int index) {
    try {
        if (const TaipanValue* v = SetterValue(call, index)) call->strings.push_back(v->toString());
        else if (IsSetter(call) || index < 0 || index >= call->args->count()) call->strings.push_back("null");
        else call->strings.push_back((*call->args)[index].toString());
        return call->strings.back().c_str();
    } catch (...) {
        return "";
    }
}

Vec2 ArgVector2 (Call* call, int index) {
    const V2 v = ReadValue<V2>(call, index, "Vector2");
    return Vec2{ v.x, v.y };
}

Vec3 ArgVector3 (Call* call, int index) {
    const V3 v = ReadValue<V3>(call, index, "Vector3");
    return Vec3{ v.x, v.y, v.z };
}

Quat ArgQuaternion (Call* call, int index) {
    const Q4 v = ReadValue<Q4>(call, index, "Quaternion");
    return Quat{ v.x, v.y, v.z, v.w };
}

Rgba ArgColor (Call* call, int index) {
    const C4 v = ReadValue<C4>(call, index, "Color");
    return Rgba{ v.r, v.g, v.b, v.a };
}

int64_t ArgInt (Call* call, int index) {
    Type t = ArgType(call, index);
    if (t != Type::Int && t != Type::Float) {
        call->RecordError(ArgLabel(index) + ": expected 'int'");
        return 0;
    }
    if (const TaipanValue* v = SetterValue(call, index)) return v->asInt();
    return call->args->getInt(index);
}

double ArgFloat (Call* call, int index) {
    Type t = ArgType(call, index);
    if (t != Type::Int && t != Type::Float) {
        call->RecordError(ArgLabel(index) + ": expected 'float'");
        return 0.0;
    }
    if (const TaipanValue* v = SetterValue(call, index)) return v->asFloat();
    return call->args->getFloat(index);
}

bool ArgBool (Call* call, int index) {
    if (ArgType(call, index) != Type::Bool) {
        call->RecordError(ArgLabel(index) + ": expected 'bool'");
        return false;
    }
    if (const TaipanValue* v = SetterValue(call, index)) return v->asBool();
    return call->args->getBool(index);
}

const char* ArgString (Call* call, int index) {
    if (ArgType(call, index) != Type::String) {
        call->RecordError(ArgLabel(index) + ": expected 'str'");
        return "";
    }
    try {
        if (const TaipanValue* v = SetterValue(call, index)) call->strings.push_back(v->asString());
        else call->strings.push_back(call->args->getString(index));
        return call->strings.back().c_str();
    } catch (...) {
        call->RecordError(ArgLabel(index) + ": could not read string");
        return "";
    }
}

void* ArgObject (Call* call, int index, const char* className) {
    Type t = ArgType(call, index);
    if (t == Type::Null) return nullptr;
    if (const TaipanValue* v = SetterValue(call, index)) {
        if (!v->isObject() || (className && v->objectClass() != className)) {
            call->RecordError(ArgLabel(index) + ": expected '" + (className ? className : "object") + "'");
            return nullptr;
        }
        return v->asObject();
    }
    try {
        return call->args->getObject(index, className);
    } catch (tp::HostError& e) {
        call->RecordError(e.message);
    } catch (...) {
        call->RecordError(ArgLabel(index) + ": expected '" + (className ? className : "object") + "'");
    }
    return nullptr;
}

void Return (Call* call, const Value& value) {
    try {
        call->result = ToTaipan(value);
    } catch (...) {
        call->RecordError("could not build the return value");
    }
}

void Fail (Call* call, const char* message) {
    try {
        call->RecordError(message ? message : "native call failed");
    } catch (...) {
        call->failed = true;
    }
}

bool Failed (const Call* call) {
    return call->failed;
}

} // namespace st::taipan
