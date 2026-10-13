#pragma once
// A small JSON value, parser and writer for the editor bridge (dev/EditorBridge.h).
//
// Exceptions are off workspace-wide and the well-known C++ JSON libraries report errors by
// throwing, so this is a ~300-line one that reports them by return value instead. It covers
// what a tool protocol needs - objects, arrays, strings with escapes and \u, numbers, bools,
// null - and nothing else (no comments, no streaming).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace st::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value () = default;
    Value (std::nullptr_t) {}
    Value (bool b) : type_(Type::Bool), bool_(b) {}
    Value (int n) : type_(Type::Number), number_(n) {}
    Value (unsigned n) : type_(Type::Number), number_(n) {}
    Value (int64_t n) : type_(Type::Number), number_(double(n)) {}
    Value (uint64_t n) : type_(Type::Number), number_(double(n)) {}
    Value (float n) : type_(Type::Number), number_(n) {}
    Value (double n) : type_(Type::Number), number_(n) {}
    Value (const char* s) : type_(Type::String), string_(s ? s : "") {}
    Value (std::string s) : type_(Type::String), string_(std::move(s)) {}

    static Value Array () { Value v; v.type_ = Type::Array; return v; }
    static Value Object () { Value v; v.type_ = Type::Object; return v; }

    Type type () const { return type_; }
    bool IsNull () const   { return type_ == Type::Null; }
    bool IsBool () const   { return type_ == Type::Bool; }
    bool IsNumber () const { return type_ == Type::Number; }
    bool IsString () const { return type_ == Type::String; }
    bool IsArray () const  { return type_ == Type::Array; }
    bool IsObject () const { return type_ == Type::Object; }

    // Reads with a fallback: the wrong type reads as the fallback, never fails.
    bool   AsBool (bool fallback = false) const      { return type_ == Type::Bool ? bool_ : fallback; }
    double AsNumber (double fallback = 0.0) const    { return type_ == Type::Number ? number_ : fallback; }
    float  AsFloat (float fallback = 0.0f) const     { return type_ == Type::Number ? float(number_) : fallback; }
    int64_t AsInt (int64_t fallback = 0) const       { return type_ == Type::Number ? int64_t(number_) : fallback; }
    const std::string& AsString () const;

    // arrays
    size_t size () const { return type_ == Type::Array ? array_.size() : (type_ == Type::Object ? object_.size() : 0); }
    // By index. A named function, not operator[]: v[0] would be ambiguous with the key
    //	lookup below (0 also converts to a null const char*).
    const Value& At (size_t i) const;
    void push_back (Value v);
    const std::vector<Value>& Items () const { return array_; }

    // objects
    bool Has (const std::string& key) const;
    const Value& operator[] (const std::string& key) const; // Null when absent
    const Value& operator[] (const char* key) const { return (*this)[std::string(key)]; }
    Value& Set (const std::string& key, Value v);
    const std::map<std::string, Value>& Members () const { return object_; }

    std::string Dump () const;

private:
    void DumpTo (std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::map<std::string, Value> object_;
};

// Parse `text`. On failure returns false and says where in `error`.
bool Parse (const std::string& text, Value& out, std::string& error);

} // namespace st::json
