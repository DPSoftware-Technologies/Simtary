#include "dev/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace st::json {

namespace {

const Value kNull;
const std::string kEmpty;

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;
    int depth = 0;

    bool Fail (const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    void Skip () {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Literal (const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s.compare(i, n, word) != 0) return Fail("bad literal");
        i += n;
        return true;
    }
    static void AppendUtf8 (std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool Hex4 (uint32_t& out) {
        if (i + 4 > s.size()) return Fail("short \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        return true;
    }
    bool String (std::string& out) {
        ++i; // opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = 0;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Parse (Value& out) {
        if (++depth > 200) return Fail("nested too deep");
        Skip();
        if (i >= s.size()) return Fail("unexpected end");
        const char c = s[i];
        bool ok = true;
        if (c == '{') {
            out = Value::Object();
            ++i;
            Skip();
            if (i < s.size() && s[i] == '}') { ++i; --depth; return true; }
            while (ok) {
                Skip();
                if (i >= s.size() || s[i] != '"') return Fail("expected key");
                std::string key;
                if (!String(key)) return false;
                Skip();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                Value v;
                if (!Parse(v)) return false;
                out.Set(key, std::move(v));
                Skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return Fail("expected ',' or '}'");
            }
        } else if (c == '[') {
            out = Value::Array();
            ++i;
            Skip();
            if (i < s.size() && s[i] == ']') { ++i; --depth; return true; }
            while (ok) {
                Value v;
                if (!Parse(v)) return false;
                out.push_back(std::move(v));
                Skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                return Fail("expected ',' or ']'");
            }
        } else if (c == '"') {
            std::string str;
            if (!String(str)) return false;
            out = Value(std::move(str));
        } else if (c == 't') {
            if (!Literal("true")) return false;
            out = Value(true);
        } else if (c == 'f') {
            if (!Literal("false")) return false;
            out = Value(false);
        } else if (c == 'n') {
            if (!Literal("null")) return false;
            out = Value();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s.c_str() + i;
            char* end = nullptr;
            const double d = std::strtod(begin, &end);
            if (end == begin) return Fail("bad number");
            i += size_t(end - begin);
            out = Value(d);
        } else {
            return Fail("unexpected character");
        }
        --depth;
        return true;
    }
};

void Escape (const std::string& in, std::string& out) {
    out += '"';
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

} // namespace

const std::string& Value::AsString () const { return type_ == Type::String ? string_ : kEmpty; }

const Value& Value::At (size_t i) const {
    return (type_ == Type::Array && i < array_.size()) ? array_[i] : kNull;
}

void Value::push_back (Value v) {
    if (type_ != Type::Array) { *this = Array(); }
    array_.push_back(std::move(v));
}

bool Value::Has (const std::string& key) const {
    return type_ == Type::Object && object_.count(key) != 0;
}

const Value& Value::operator[] (const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    auto it = object_.find(key);
    return it == object_.end() ? kNull : it->second;
}

Value& Value::Set (const std::string& key, Value v) {
    if (type_ != Type::Object) { *this = Object(); }
    Value& slot = object_[key];
    slot = std::move(v);
    return slot;
}

void Value::DumpTo (std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(number_)) { out += "null"; break; }
            char buf[40];
            if (number_ == std::floor(number_) && std::fabs(number_) < 1e15)
                std::snprintf(buf, sizeof(buf), "%.0f", number_);
            else
                std::snprintf(buf, sizeof(buf), "%.9g", number_);
            out += buf;
            break;
        }
        case Type::String: Escape(string_, out); break;
        case Type::Array: {
            out += '[';
            for (size_t k = 0; k < array_.size(); ++k) {
                if (k) out += ',';
                array_[k].DumpTo(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : object_) {
                if (!first) out += ',';
                first = false;
                Escape(kv.first, out);
                out += ':';
                kv.second.DumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Value::Dump () const {
    std::string out;
    DumpTo(out);
    return out;
}

bool Parse (const std::string& text, Value& out, std::string& error) {
    Parser p{ text };
    if (!p.Parse(out)) {
        error = p.error;
        return false;
    }
    p.Skip();
    if (p.i != text.size()) {
        error = "trailing characters at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

} // namespace st::json
