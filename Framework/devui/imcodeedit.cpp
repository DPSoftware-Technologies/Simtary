#include "devui/imcodeedit.h"

#include "imgui_internal.h" // WantTextInputNextFrame / PlatformImeData, window inner rect

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace st::devui {

namespace {

// ---------------------------------------------------------------------------
// Taipan's vocabulary (libs/Taipan/docs.md, "Keywords")
// ---------------------------------------------------------------------------
const char* const kKeywords[] = {
    "func", "coroutine", "start", "return", "if", "else", "for", "in", "while", "match", "break",
    "continue", "try", "catch", "throw", "class", "interface", "implements", "enum", "import", "as",
    "enable", "const", "private", "public", "wait",
};
const char* const kConstants[] = { "true", "false", "null", "this" };
const char* const kTypes[] = { "any", "int", "float", "bool", "str", "list", "dict", "void" };

bool InList (const std::string& w, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (w == list[i]) return true;
    return false;
}
bool IsKeyword (const std::string& w) { return InList(w, kKeywords, sizeof(kKeywords) / sizeof(*kKeywords)); }
bool IsConstant (const std::string& w) { return InList(w, kConstants, sizeof(kConstants) / sizeof(*kConstants)); }
bool IsBuiltinType (const std::string& w) { return InList(w, kTypes, sizeof(kTypes) / sizeof(*kTypes)); }

bool IdentStart (unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool IdentChar (unsigned char c) { return IdentStart(c) || (c >= '0' && c <= '9'); }
bool Continuation (unsigned char c) { return (c & 0xC0) == 0x80; }
// A typed codepoint (not a byte): anything past ASCII counts as a letter.
bool IdentCodepoint (unsigned int ch) { return ch >= 0x80 || IdentChar((unsigned char)ch); }

// ---------------------------------------------------------------------------
// colours (dark theme, close to what most editors ship)
// ---------------------------------------------------------------------------
enum class Tok { Text, Keyword, Constant, Type, String, Number, Comment, Function, Namespace, Punct, Interp };

ImU32 TokColour (Tok t) {
    switch (t) {
    case Tok::Keyword:   return IM_COL32(197, 134, 192, 255);
    case Tok::Constant:  return IM_COL32(86, 156, 214, 255);
    case Tok::Type:      return IM_COL32(78, 201, 176, 255);
    case Tok::String:    return IM_COL32(206, 145, 120, 255);
    case Tok::Number:    return IM_COL32(181, 206, 168, 255);
    case Tok::Comment:   return IM_COL32(106, 153, 85, 255);
    case Tok::Function:  return IM_COL32(220, 220, 170, 255);
    case Tok::Namespace: return IM_COL32(78, 201, 176, 255);
    case Tok::Punct:     return IM_COL32(180, 180, 180, 255);
    case Tok::Interp:    return IM_COL32(86, 156, 214, 255);
    default:             return IM_COL32(212, 212, 212, 255);
    }
}

struct Span {
    int from, to;
    Tok tok;
};

// One line into coloured spans. `inBlock` carries a /* comment */ across lines.
void Tokenize (const std::string& s, bool& inBlock, std::vector<Span>& out,
               const std::set<std::string>& types, const CodeEditor::Api* api) {
    out.clear();
    const int n = (int)s.size();
    int i = 0;
    while (i < n) {
        if (inBlock) {
            const size_t end = s.find("*/", (size_t)i);
            const int to = end == std::string::npos ? n : (int)end + 2;
            out.push_back({ i, to, Tok::Comment });
            if (end != std::string::npos) inBlock = false;
            i = to;
            continue;
        }
        const unsigned char c = (unsigned char)s[i];
        const unsigned char next = i + 1 < n ? (unsigned char)s[i + 1] : 0;
        if (c == '/' && next == '/') {
            out.push_back({ i, n, Tok::Comment });
            break;
        }
        if (c == '/' && next == '*') {
            inBlock = true;
            continue;
        }
        const bool fstring = c == 'f' && next == '"' && (i == 0 || !IdentChar((unsigned char)s[i - 1]));
        if (c == '"' || fstring) {
            int j = i + (fstring ? 2 : 1);
            int segment = i;
            int depth = 0;
            while (j < n) {
                const char d = s[j];
                if (d == '\\') { j += 2; continue; }
                if (fstring && d == '{') {
                    if (depth == 0) {
                        out.push_back({ segment, j, Tok::String });
                        out.push_back({ j, j + 1, Tok::Interp });
                        segment = j + 1;
                    }
                    ++depth;
                } else if (fstring && d == '}' && depth > 0) {
                    if (--depth == 0) {
                        out.push_back({ segment, j, Tok::Text });
                        out.push_back({ j, j + 1, Tok::Interp });
                        segment = j + 1;
                    }
                } else if (d == '"' && depth == 0) {
                    ++j;
                    break;
                }
                ++j;
            }
            j = std::min(j, n);
            out.push_back({ segment, j, Tok::String });
            i = j;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && next >= '0' && next <= '9' && (i == 0 || !IdentChar((unsigned char)s[i - 1])))) {
            int j = i + 1;
            while (j < n && (IdentChar((unsigned char)s[j]) || s[j] == '.')) ++j;
            out.push_back({ i, j, Tok::Number });
            i = j;
            continue;
        }
        if (IdentStart(c)) {
            int j = i + 1;
            while (j < n && IdentChar((unsigned char)s[j])) ++j;
            const std::string w = s.substr((size_t)i, (size_t)(j - i));
            int k = j;
            while (k < n && s[k] == ' ') ++k;
            const char after = k < n ? s[k] : 0;
            const bool member = i > 0 && s[i - 1] == '.';
            Tok t = Tok::Text;
            if (!member && IsKeyword(w))                                    t = Tok::Keyword;
            else if (!member && IsConstant(w))                              t = Tok::Constant;
            else if (!member && (IsBuiltinType(w) || types.count(w)))       t = Tok::Type;
            else if (!member && api && after == '.' && api->namespaces.count(w)) t = Tok::Namespace;
            else if (after == '(')                                          t = Tok::Function;
            out.push_back({ i, j, t });
            i = j;
            continue;
        }
        // UTF-8 continuation bytes and anything else: one punctuation run
        int j = i + 1;
        while (j < n && Continuation((unsigned char)s[j])) ++j;
        out.push_back({ i, j, (c == ' ' || c == '\t') ? Tok::Text : Tok::Punct });
        i = j;
    }
}

int LeadingSpaces (const std::string& s) {
    int i = 0;
    while (i < (int)s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

bool StartsWithNoCase (const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    return true;
}

bool ContainsNoCase (const std::string& s, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(s.begin(), s.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != s.end();
}

} // namespace

// ---------------------------------------------------------------------------
// text
// ---------------------------------------------------------------------------
void CodeEditor::SetText (const std::string& text) {
    lines_.clear();
    std::string current;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            lines_.push_back(std::move(current));
            current.clear();
        } else if (c == '\t') {
            current += "    ";
        } else {
            current += c;
        }
    }
    lines_.push_back(std::move(current));
    cursor_ = anchor_ = Pos{};
    undo_.clear();
    redo_.clear();
    completionOpen_ = false;
    ++version_;
}

std::string CodeEditor::GetText () const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += '\n';
        out += lines_[i];
    }
    return out;
}

int CodeEditor::CursorColumn () const {
    const std::string& s = lines_[cursor_.line];
    int chars = 1;
    for (int i = 0; i < cursor_.col && i < (int)s.size(); ++i)
        if (!Continuation((unsigned char)s[i])) ++chars;
    return chars;
}

void CodeEditor::JumpTo (int line, int column) {
    cursor_.line = std::clamp(line - 1, 0, (int)lines_.size() - 1);
    cursor_.col = std::clamp(column - 1, 0, (int)lines_[cursor_.line].size());
    anchor_ = cursor_;
    scrollToCursor_ = true;
    wantFocus_ = true;
    completionOpen_ = false;
}

void CodeEditor::Clamp (Pos& p) const {
    p.line = std::clamp(p.line, 0, (int)lines_.size() - 1);
    p.col = std::clamp(p.col, 0, (int)lines_[p.line].size());
}

std::string CodeEditor::SelectedText () const {
    const Pos a = SelStart(), b = SelEnd();
    if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
    std::string out = lines_[a.line].substr(a.col);
    for (int l = a.line + 1; l < b.line; ++l) out += "\n" + lines_[l];
    out += "\n" + lines_[b.line].substr(0, b.col);
    return out;
}

void CodeEditor::DeleteSelection () {
    if (!HasSelection()) return;
    const Pos a = SelStart(), b = SelEnd();
    lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
    if (b.line > a.line) lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    cursor_ = anchor_ = a;
}

void CodeEditor::Insert (const std::string& text) {
    DeleteSelection();
    std::string& line = lines_[cursor_.line];
    const std::string tail = line.substr(cursor_.col);
    line.erase(cursor_.col);
    size_t start = 0;
    bool first = true;
    while (true) {
        const size_t nl = text.find('\n', start);
        const std::string piece = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (first) {
            lines_[cursor_.line] += piece;
            first = false;
        } else {
            lines_.insert(lines_.begin() + cursor_.line + 1, piece);
            ++cursor_.line;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    cursor_.col = (int)lines_[cursor_.line].size();
    lines_[cursor_.line] += tail;
    anchor_ = cursor_;
}

void CodeEditor::PushUndo (bool mergeTyping) {
    const double now = ImGui::GetTime();
    // Consecutive typing within a second is one undo step, like every editor.
    const bool merge = mergeTyping && lastWasTyping_ && now - lastTypingTime_ < 1.0 && !undo_.empty();
    if (!merge) {
        undo_.push_back(Snapshot{ lines_, cursor_, anchor_ });
        if (undo_.size() > 400) undo_.erase(undo_.begin());
    }
    redo_.clear();
    lastWasTyping_ = mergeTyping;
    lastTypingTime_ = now;
}

void CodeEditor::Undo () {
    if (undo_.empty()) return;
    redo_.push_back(Snapshot{ lines_, cursor_, anchor_ });
    lines_ = undo_.back().lines;
    cursor_ = undo_.back().cursor;
    anchor_ = undo_.back().anchor;
    undo_.pop_back();
    lastWasTyping_ = false;
    Edited();
}

void CodeEditor::Redo () {
    if (redo_.empty()) return;
    undo_.push_back(Snapshot{ lines_, cursor_, anchor_ });
    lines_ = redo_.back().lines;
    cursor_ = redo_.back().cursor;
    anchor_ = redo_.back().anchor;
    redo_.pop_back();
    lastWasTyping_ = false;
    Edited();
}

void CodeEditor::Edited () {
    ++version_;
    Clamp(cursor_);
    Clamp(anchor_);
    scrollToCursor_ = true;
    blinkStart_ = (float)ImGui::GetTime();
    preferredX_ = -1.0f;
}

// ---------------------------------------------------------------------------
// movement
// ---------------------------------------------------------------------------
CodeEditor::Pos CodeEditor::Left (Pos p, bool word) const {
    if (p.col == 0) {
        if (p.line > 0) { --p.line; p.col = (int)lines_[p.line].size(); }
        return p;
    }
    const std::string& s = lines_[p.line];
    if (!word) {
        do { --p.col; } while (p.col > 0 && Continuation((unsigned char)s[p.col]));
        return p;
    }
    while (p.col > 0 && s[p.col - 1] == ' ') --p.col;
    if (p.col > 0 && IdentChar((unsigned char)s[p.col - 1])) {
        while (p.col > 0 && IdentChar((unsigned char)s[p.col - 1])) --p.col;
    } else if (p.col > 0) {
        --p.col;
    }
    return p;
}

CodeEditor::Pos CodeEditor::Right (Pos p, bool word) const {
    const std::string& s = lines_[p.line];
    if (p.col >= (int)s.size()) {
        if (p.line + 1 < (int)lines_.size()) { ++p.line; p.col = 0; }
        return p;
    }
    if (!word) {
        do { ++p.col; } while (p.col < (int)s.size() && Continuation((unsigned char)s[p.col]));
        return p;
    }
    if (IdentChar((unsigned char)s[p.col])) {
        while (p.col < (int)s.size() && IdentChar((unsigned char)s[p.col])) ++p.col;
    } else {
        ++p.col;
    }
    while (p.col < (int)s.size() && s[p.col] == ' ') ++p.col;
    return p;
}

void CodeEditor::MoveTo (Pos p, bool select) {
    Clamp(p);
    cursor_ = p;
    if (!select) anchor_ = p;
    scrollToCursor_ = true;
    blinkStart_ = (float)ImGui::GetTime();
}

void CodeEditor::SelectWordAt (Pos p) {
    Clamp(p);
    const std::string& s = lines_[p.line];
    int a = p.col, b = p.col;
    while (a > 0 && IdentChar((unsigned char)s[a - 1])) --a;
    while (b < (int)s.size() && IdentChar((unsigned char)s[b])) ++b;
    if (a == b && b < (int)s.size()) ++b;
    anchor_ = Pos{ p.line, a };
    cursor_ = Pos{ p.line, b };
}

// ---------------------------------------------------------------------------
// editing features
// ---------------------------------------------------------------------------
void CodeEditor::NewLine () {
    PushUndo(false);
    DeleteSelection();
    const std::string& line = lines_[cursor_.line];
    std::string indent = line.substr(0, std::min(LeadingSpaces(line), cursor_.col));
    // ignore trailing spaces when looking at what the line ends with
    int before = cursor_.col - 1;
    while (before >= 0 && line[before] == ' ') --before;
    const bool opens = before >= 0 && (line[before] == '{' || line[before] == '(' || line[before] == '[');
    const bool closes = cursor_.col < (int)line.size() &&
                        (line[cursor_.col] == '}' || line[cursor_.col] == ')' || line[cursor_.col] == ']');
    if (opens && closes) {
        // "{|}" -> the body on its own line, the brace under the opener
        Insert("\n" + indent + "    \n" + indent);
        cursor_.line -= 1;
        cursor_.col = (int)indent.size() + 4;
        anchor_ = cursor_;
    } else {
        Insert("\n" + indent + (opens ? "    " : ""));
    }
    Edited();
}

void CodeEditor::Indent (bool outdent) {
    PushUndo(false);
    const int a = SelStart().line, b = SelEnd().line - ((SelEnd().col == 0 && SelEnd().line > a) ? 1 : 0);
    for (int l = a; l <= b; ++l) {
        std::string& s = lines_[l];
        if (outdent) {
            const int n = std::min(4, LeadingSpaces(s));
            s.erase(0, n);
            if (l == cursor_.line) cursor_.col = std::max(0, cursor_.col - n);
            if (l == anchor_.line) anchor_.col = std::max(0, anchor_.col - n);
        } else {
            s.insert(0, "    ");
            if (l == cursor_.line && cursor_.col > 0) cursor_.col += 4;
            if (l == anchor_.line && anchor_.col > 0) anchor_.col += 4;
        }
    }
    Edited();
}

void CodeEditor::ToggleComment () {
    PushUndo(false);
    const int a = SelStart().line, b = SelEnd().line;
    bool allCommented = true;
    for (int l = a; l <= b; ++l) {
        const std::string& s = lines_[l];
        const int lead = LeadingSpaces(s);
        if (lead < (int)s.size() && s.compare(lead, 2, "//") != 0) allCommented = false;
    }
    for (int l = a; l <= b; ++l) {
        std::string& s = lines_[l];
        const int lead = LeadingSpaces(s);
        if (allCommented) {
            if (s.compare(lead, 3, "// ") == 0) s.erase(lead, 3);
            else if (s.compare(lead, 2, "//") == 0) s.erase(lead, 2);
        } else if (lead < (int)s.size() || a == b) {
            s.insert(lead, "// ");
        }
    }
    Edited();
}

// ---------------------------------------------------------------------------
// completion
// ---------------------------------------------------------------------------
std::string CodeEditor::WordBeforeCursor (std::string* owner) const {
    const std::string& s = lines_[cursor_.line];
    int a = cursor_.col;
    while (a > 0 && IdentChar((unsigned char)s[a - 1])) --a;
    if (owner) {
        owner->clear();
        if (a > 0 && s[a - 1] == '.') {
            // the dotted chain in front: "util.mathx." -> "util.mathx", "self." -> "self"
            int b = a - 1;
            int c = b;
            while (c > 0 && (IdentChar((unsigned char)s[c - 1]) || s[c - 1] == '.')) --c;
            *owner = s.substr(c, b - c);
        }
    }
    return s.substr(a, cursor_.col - a);
}

std::string CodeEditor::TypeOfVariable (const std::string& name) const {
    auto it = varTypes_.find(name);
    if (it == varTypes_.end()) return std::string();
    if (api_) {
        auto alias = api_->aliases.find(it->second);
        if (alias != api_->aliases.end()) return alias->second;
    }
    return it->second;
}

void CodeEditor::UpdateCompletion (bool force) {
    std::string owner;
    const std::string prefix = WordBeforeCursor(&owner);
    candidates_.clear();

    // Not inside a string or a comment - typing prose there is not asking for code. The
    //	{expressions} of an f-string are code, and tokenize as such.
    if (!force) {
        bool inBlock = false;
        std::vector<Span> spans;
        static const std::set<std::string> none;
        for (int l = 0; l < cursor_.line; ++l) Tokenize(lines_[l], inBlock, spans, none, nullptr);
        Tokenize(lines_[cursor_.line], inBlock, spans, none, nullptr);
        for (const Span& sp : spans) {
            if (sp.from < cursor_.col && cursor_.col <= sp.to && (sp.tok == Tok::String || sp.tok == Tok::Comment)) {
                completionOpen_ = false;
                return;
            }
        }
    }

    auto add = [&](const std::string& name, const std::string& detail, bool callable, int kind) {
        if (name.empty() || name == prefix) return;
        if (!StartsWithNoCase(name, prefix) && !(prefix.size() >= 2 && ContainsNoCase(name, prefix))) return;
        for (const Candidate& c : candidates_) if (c.name == name) return;
        candidates_.push_back(Candidate{ name, detail, callable, kind });
    };

    if (!owner.empty()) {
        // members: a namespace, or a variable of a known class
        const std::vector<CodeEditor::Symbol>* members = nullptr;
        if (api_) {
            auto ns = api_->namespaces.find(owner);
            if (ns != api_->namespaces.end()) members = &ns->second;
            if (!members) {
                const std::string type = TypeOfVariable(owner.substr(owner.find_last_of('.') == std::string::npos ? 0 : owner.find_last_of('.') + 1));
                auto cls = api_->classes.find(type);
                if (cls != api_->classes.end()) members = &cls->second;
            }
        }
        if (members)
            for (const Symbol& m : *members) add(m.name, m.detail, m.callable, m.callable ? 2 : 4);
    } else if (force || !prefix.empty()) {
        // a fresh word: locals and declarations first, then the language, then the API
        for (const Candidate& c : scriptNames_) add(c.name, c.detail, c.callable, c.kind);
        for (const char* k : kKeywords) add(k, "keyword", false, 1);
        for (const char* k : kConstants) add(k, "", false, 1);
        for (const char* k : kTypes) add(k, "type", false, 3);
        for (const std::string& t : scriptTypes_) add(t, "class", false, 3);
        if (api_) {
            for (const auto& kv : api_->classes) add(kv.first, "class", false, 3);
            for (const auto& kv : api_->namespaces) add(kv.first, "namespace", false, 3);
            for (const Symbol& g : api_->globals) add(g.name, g.detail, g.callable, 2);
        }
    }

    // Prefix matches before substring matches, then case-exact first, then A-Z.
    std::stable_sort(candidates_.begin(), candidates_.end(), [&](const Candidate& a, const Candidate& b) {
        const bool pa = StartsWithNoCase(a.name, prefix), pb = StartsWithNoCase(b.name, prefix);
        if (pa != pb) return pa;
        const bool ea = a.name.compare(0, prefix.size(), prefix) == 0, eb = b.name.compare(0, prefix.size(), prefix) == 0;
        if (ea != eb) return ea;
        return a.name < b.name;
    });

    completionOpen_ = !candidates_.empty();
    completionIndex_ = 0;
    completionScroll_ = 0;
    completionPrefixLen_ = (int)prefix.size();
}

void CodeEditor::AcceptCompletion () {
    if (!completionOpen_ || completionIndex_ < 0 || completionIndex_ >= (int)candidates_.size()) return;
    const Candidate c = candidates_[completionIndex_];
    PushUndo(false);
    anchor_ = cursor_;
    anchor_.col -= completionPrefixLen_;
    std::string text = c.name;
    const std::string& line = lines_[cursor_.line];
    const bool parenFollows = cursor_.col < (int)line.size() && line[cursor_.col] == '(';
    if (c.callable && !parenFollows) {
        // "()" for a call; the cursor lands inside unless it takes no arguments
        const bool noArgs = c.detail.compare(0, 2, "()") == 0;
        Insert(text + "()");
        if (!noArgs) { --cursor_.col; anchor_ = cursor_; }
    } else {
        Insert(text);
    }
    completionOpen_ = false;
    Edited();
}

void CodeEditor::DrawCompletion (const ImVec2& at, float lineHeight) {
    popupHovered_ = false;
    if (!completionOpen_ || candidates_.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const int visible = std::min(10, (int)candidates_.size());
    completionIndex_ = std::clamp(completionIndex_, 0, (int)candidates_.size() - 1);
    if (completionIndex_ < completionScroll_) completionScroll_ = completionIndex_;
    if (completionIndex_ >= completionScroll_ + visible) completionScroll_ = completionIndex_ - visible + 1;

    float nameW = 120.0f, detailW = 0.0f;
    for (int i = completionScroll_; i < completionScroll_ + visible; ++i) {
        nameW = std::max(nameW, ImGui::CalcTextSize(candidates_[i].name.c_str()).x);
        detailW = std::max(detailW, std::min(260.0f, ImGui::CalcTextSize(candidates_[i].detail.c_str()).x));
    }
    const float pad = 6.0f, icon = lineHeight * 0.55f;
    const float width = pad * 3 + icon + nameW + (detailW > 0 ? detailW + pad * 2 : 0);
    const float rowH = lineHeight;
    ImVec2 p0(at.x - icon - pad * 2, at.y + lineHeight);
    // keep it on screen: flip above the line when there is no room below
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (p0.y + rowH * visible + pad * 2 > display.y) p0.y = at.y - rowH * visible - pad * 2;
    if (p0.x + width > display.x) p0.x = std::max(0.0f, display.x - width);
    const ImVec2 p1(p0.x + width, p0.y + rowH * visible + pad);
    popupMin_ = p0;
    popupMax_ = p1;

    dl->AddRectFilled(p0, p1, IM_COL32(37, 37, 38, 250), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(69, 69, 69, 255), 4.0f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    popupHovered_ = mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y;

    for (int i = completionScroll_; i < completionScroll_ + visible; ++i) {
        const Candidate& c = candidates_[i];
        const float y = p0.y + pad * 0.5f + (i - completionScroll_) * rowH;
        const bool rowHovered = popupHovered_ && mouse.y >= y && mouse.y < y + rowH;
        if (i == completionIndex_)
            dl->AddRectFilled(ImVec2(p0.x + 2, y), ImVec2(p1.x - 2, y + rowH), IM_COL32(4, 57, 94, 255), 3.0f);
        else if (rowHovered)
            dl->AddRectFilled(ImVec2(p0.x + 2, y), ImVec2(p1.x - 2, y + rowH), IM_COL32(55, 55, 61, 255), 3.0f);
        // kind dot
        static const ImU32 kindColour[5] = { IM_COL32(150, 150, 150, 255), IM_COL32(197, 134, 192, 255),
                                             IM_COL32(220, 220, 170, 255), IM_COL32(78, 201, 176, 255),
                                             IM_COL32(86, 156, 214, 255) };
        dl->AddRectFilled(ImVec2(p0.x + pad, y + rowH * 0.25f), ImVec2(p0.x + pad + icon, y + rowH * 0.75f),
                          kindColour[std::clamp(c.kind, 0, 4)], 2.0f);
        const float tx = p0.x + pad * 2 + icon;
        const float ty = y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        // the typed part in bold-ish blue, like VS Code
        const int from = std::max(0, cursor_.col - completionPrefixLen_);
        const std::string typed = lines_[cursor_.line].substr((size_t)from, (size_t)(cursor_.col - from));
        const int match = StartsWithNoCase(c.name, typed) ? (int)typed.size() : 0;
        if (match > 0) dl->AddText(ImVec2(tx, ty), IM_COL32(24, 163, 255, 255), c.name.c_str(), c.name.c_str() + match);
        dl->AddText(ImVec2(tx + ImGui::CalcTextSize(c.name.c_str(), c.name.c_str() + match).x, ty),
                    IM_COL32(212, 212, 212, 255), c.name.c_str() + match);
        if (!c.detail.empty()) {
            dl->PushClipRect(ImVec2(tx + nameW + pad, y), ImVec2(p1.x - pad, y + rowH), true);
            dl->AddText(ImVec2(tx + nameW + pad * 2, ty), IM_COL32(140, 140, 140, 255), c.detail.c_str());
            dl->PopClipRect();
        }
        if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            completionIndex_ = i;
            AcceptCompletion();
            return;
        }
    }
    if (popupHovered_ && ImGui::GetIO().MouseWheel != 0.0f) {
        completionIndex_ = std::clamp(completionIndex_ - (int)ImGui::GetIO().MouseWheel, 0, (int)candidates_.size() - 1);
    }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
void CodeEditor::HandleKeyboard () {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    // completion owns the navigation keys while it is open
    if (completionOpen_) {
        if (pressed(ImGuiKey_Escape)) { completionOpen_ = false; return; }
        if (pressed(ImGuiKey_DownArrow)) { completionIndex_ = (completionIndex_ + 1) % (int)candidates_.size(); return; }
        if (pressed(ImGuiKey_UpArrow)) {
            completionIndex_ = (completionIndex_ + (int)candidates_.size() - 1) % (int)candidates_.size();
            return;
        }
        if (pressed(ImGuiKey_PageDown)) { completionIndex_ = std::min(completionIndex_ + 9, (int)candidates_.size() - 1); return; }
        if (pressed(ImGuiKey_PageUp)) { completionIndex_ = std::max(completionIndex_ - 9, 0); return; }
        if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Tab)) {
            AcceptCompletion();
            return;
        }
    }

    const int pageLines = std::max(1, (int)(ImGui::GetWindowHeight() / ImGui::GetTextLineHeightWithSpacing()) - 2);

    if (ctrl && !alt) {
        if (pressed(ImGuiKey_Space)) { UpdateCompletion(true); return; }
        if (pressed(ImGuiKey_A)) { anchor_ = Pos{ 0, 0 }; cursor_ = Pos{ (int)lines_.size() - 1, (int)lines_.back().size() }; return; }
        if (pressed(ImGuiKey_Z)) { shift ? Redo() : Undo(); completionOpen_ = false; return; }
        if (pressed(ImGuiKey_Y)) { Redo(); completionOpen_ = false; return; }
        if (pressed(ImGuiKey_Slash)) { ToggleComment(); return; }
        if (pressed(ImGuiKey_C) || pressed(ImGuiKey_X)) {
            const bool cut = ImGui::IsKeyPressed(ImGuiKey_X, false);
            if (HasSelection()) {
                ImGui::SetClipboardText(SelectedText().c_str());
                if (cut) { PushUndo(false); DeleteSelection(); Edited(); }
            } else {
                // no selection: the whole line, as editors do
                ImGui::SetClipboardText((lines_[cursor_.line] + "\n").c_str());
                if (cut) {
                    PushUndo(false);
                    if (lines_.size() > 1) lines_.erase(lines_.begin() + cursor_.line);
                    else lines_[0].clear();
                    cursor_.col = 0;
                    anchor_ = cursor_;
                    Edited();
                }
            }
            return;
        }
        if (pressed(ImGuiKey_V)) {
            if (const char* clip = ImGui::GetClipboardText()) {
                std::string text;
                for (const char* p = clip; *p; ++p) {
                    if (*p == '\r') continue;
                    if (*p == '\t') text += "    ";
                    else text += *p;
                }
                PushUndo(false);
                Insert(text);
                Edited();
            }
            completionOpen_ = false;
            return;
        }
        if (pressed(ImGuiKey_D)) {
            PushUndo(false);
            lines_.insert(lines_.begin() + cursor_.line + 1, lines_[cursor_.line]);
            ++cursor_.line;
            anchor_ = cursor_;
            Edited();
            return;
        }
    }

    // movement
    auto vertical = [&](int delta) {
        if (preferredX_ < 0.0f) preferredX_ = TextWidth(lines_[cursor_.line], 0, cursor_.col);
        const float keep = preferredX_;
        Pos p = cursor_;
        p.line = std::clamp(p.line + delta, 0, (int)lines_.size() - 1);
        p.col = ColumnAt(p.line, keep);
        MoveTo(p, shift);
        preferredX_ = keep;
        completionOpen_ = false;
    };
    if (pressed(ImGuiKey_LeftArrow)) {
        MoveTo(HasSelection() && !shift ? SelStart() : Left(cursor_, ctrl), shift);
        preferredX_ = -1.0f; completionOpen_ = false; return;
    }
    if (pressed(ImGuiKey_RightArrow)) {
        MoveTo(HasSelection() && !shift ? SelEnd() : Right(cursor_, ctrl), shift);
        preferredX_ = -1.0f; completionOpen_ = false; return;
    }
    if (pressed(ImGuiKey_UpArrow))   { vertical(-1); return; }
    if (pressed(ImGuiKey_DownArrow)) { vertical(1); return; }
    if (pressed(ImGuiKey_PageUp))    { vertical(-pageLines); return; }
    if (pressed(ImGuiKey_PageDown))  { vertical(pageLines); return; }
    if (pressed(ImGuiKey_Home)) {
        // first press: to the indentation; again: to column 0
        const int lead = LeadingSpaces(lines_[cursor_.line]);
        Pos p = ctrl ? Pos{ 0, 0 } : Pos{ cursor_.line, cursor_.col == lead ? 0 : lead };
        MoveTo(p, shift); preferredX_ = -1.0f; completionOpen_ = false; return;
    }
    if (pressed(ImGuiKey_End)) {
        Pos p = ctrl ? Pos{ (int)lines_.size() - 1, (int)lines_.back().size() }
                     : Pos{ cursor_.line, (int)lines_[cursor_.line].size() };
        MoveTo(p, shift); preferredX_ = -1.0f; completionOpen_ = false; return;
    }

    // editing keys
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) { NewLine(); completionOpen_ = false; return; }
    if (pressed(ImGuiKey_Tab)) {
        if (shift) Indent(true);
        else if (HasSelection() && SelStart().line != SelEnd().line) Indent(false);
        else {
            PushUndo(false);
            const int col = cursor_.col;
            Insert(std::string(4 - (col % 4), ' '));
            Edited();
        }
        return;
    }
    if (pressed(ImGuiKey_Backspace)) {
        PushUndo(true);
        if (!HasSelection()) {
            const std::string& s = lines_[cursor_.line];
            const int lead = LeadingSpaces(s);
            if (!ctrl && cursor_.col > 0 && cursor_.col <= lead) {
                // in the indentation: back to the previous tab stop
                anchor_ = Pos{ cursor_.line, ((cursor_.col - 1) / 4) * 4 };
            } else if (!ctrl && cursor_.col > 0 && cursor_.col < (int)s.size() &&
                       std::strchr("([{\"", s[cursor_.col - 1]) &&
                       s[cursor_.col] == (s[cursor_.col - 1] == '(' ? ')' : s[cursor_.col - 1] == '[' ? ']'
                                         : s[cursor_.col - 1] == '{' ? '}' : '"')) {
                // "(|)" -> both go
                anchor_ = Pos{ cursor_.line, cursor_.col - 1 };
                cursor_.col += 1;
            } else {
                anchor_ = Left(cursor_, ctrl);
            }
        }
        DeleteSelection();
        Edited();
        if (completionOpen_) UpdateCompletion(false);
        return;
    }
    if (pressed(ImGuiKey_Delete)) {
        PushUndo(false);
        if (!HasSelection()) anchor_ = Right(cursor_, ctrl);
        DeleteSelection();
        Edited();
        completionOpen_ = false;
        return;
    }

    // typed characters
    if (ctrl && !alt) return; // shortcuts above; AltGr arrives as Ctrl+Alt and types
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const unsigned int ch = io.InputQueueCharacters[i];
        if (ch < 32 || ch == 127) continue;
        char utf8[5] = {};
        ImTextCharToUtf8(utf8, ch);
        const std::string& s = lines_[cursor_.line];
        const char next = cursor_.col < (int)s.size() ? s[cursor_.col] : 0;
        const char prev = cursor_.col > 0 ? s[cursor_.col - 1] : 0;

        PushUndo(IdentCodepoint(ch) || ch == ' ');
        if ((ch == ')' || ch == ']' || ch == '}' || ch == '"') && next == (char)ch && !HasSelection()) {
            // typing the closer that is already there just steps over it
            cursor_.col += 1;
            anchor_ = cursor_;
        } else if ((ch == '(' || ch == '[' || ch == '{') || (ch == '"' && !IdentChar((unsigned char)prev) && prev != '\\')) {
            const char close = ch == '(' ? ')' : ch == '[' ? ']' : ch == '{' ? '}' : '"';
            if (HasSelection() && SelStart().line == SelEnd().line) {
                const std::string inner = SelectedText();
                Insert(std::string(utf8) + inner + close);
            } else {
                Insert(std::string(utf8) + close);
                cursor_.col -= 1;
                anchor_ = cursor_;
            }
        } else if (ch == '}' && cursor_.col == LeadingSpaces(s) && cursor_.col >= 4) {
            // a closing brace on an otherwise empty line outdents itself
            lines_[cursor_.line].erase(0, 4);
            cursor_.col -= 4;
            anchor_ = cursor_;
            Insert(utf8);
        } else {
            Insert(utf8);
        }
        Edited();
        if (IdentCodepoint(ch) || ch == '.') UpdateCompletion(false);
        else completionOpen_ = false;
    }
}

void CodeEditor::HandleMouse (const ImVec2& origin, float gutter, bool hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    auto hit = [&]() {
        Pos p;
        p.line = std::clamp((int)std::floor((io.MousePos.y - origin.y) / lineH), 0, (int)lines_.size() - 1);
        p.col = ColumnAt(p.line, io.MousePos.x - (origin.x + gutter));
        return p;
    };
    // the scrollbars are part of the window but not of the text
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const bool inText = hovered && window->InnerClipRect.Contains(io.MousePos) && !popupHovered_;

    if (inText && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        focused_ = true;
        completionOpen_ = false;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            SelectWordAt(hit());
        } else {
            MoveTo(hit(), io.KeyShift);
            dragging_ = true;
        }
        preferredX_ = -1.0f;
    }
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            cursor_ = hit();
            scrollToCursor_ = true;
        } else {
            dragging_ = false;
        }
    }
    if (inText) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
float CodeEditor::TextWidth (const std::string& line, int fromCol, int toCol) const {
    fromCol = std::clamp(fromCol, 0, (int)line.size());
    toCol = std::clamp(toCol, fromCol, (int)line.size());
    if (fromCol == toCol) return 0.0f;
    return ImGui::CalcTextSize(line.c_str() + fromCol, line.c_str() + toCol).x;
}

int CodeEditor::ColumnAt (int line, float x) const {
    const std::string& s = lines_[std::clamp(line, 0, (int)lines_.size() - 1)];
    if (x <= 0.0f) return 0;
    float acc = 0.0f;
    int i = 0;
    while (i < (int)s.size()) {
        int j = i + 1;
        while (j < (int)s.size() && Continuation((unsigned char)s[j])) ++j;
        const float w = ImGui::CalcTextSize(s.c_str() + i, s.c_str() + j).x;
        if (acc + w * 0.5f > x) return i;
        acc += w;
        i = j;
    }
    return (int)s.size();
}

void CodeEditor::Reindex () {
    indexedVersion_ = version_;
    scriptTypes_.clear();
    scriptNames_.clear();
    varTypes_.clear();
    maxWidth_ = 0.0f;

    // Words in order, with strings and comments skipped, plus the first char after each.
    struct Word { std::string text; char after; };
    std::vector<Word> words;
    bool inBlock = false;
    for (const std::string& s : lines_) {
        maxWidth_ = std::max(maxWidth_, ImGui::CalcTextSize(s.c_str()).x);
        const int n = (int)s.size();
        int i = 0;
        while (i < n) {
            if (inBlock) {
                const size_t end = s.find("*/", (size_t)i);
                if (end == std::string::npos) { i = n; break; }
                inBlock = false;
                i = (int)end + 2;
                continue;
            }
            const char c = s[i];
            if (c == '/' && i + 1 < n && s[i + 1] == '/') break;
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { inBlock = true; i += 2; continue; }
            if (c == '"') {
                int j = i + 1;
                while (j < n && s[j] != '"') j += (s[j] == '\\') ? 2 : 1;
                i = j + 1;
                continue;
            }
            if (IdentStart((unsigned char)c)) {
                int j = i + 1;
                while (j < n && IdentChar((unsigned char)s[j])) ++j;
                int k = j;
                while (k < n && s[k] == ' ') ++k;
                // "f" right before a string is the f-string prefix, not a word
                if (!(j - i == 1 && c == 'f' && j < n && s[j] == '"'))
                    words.push_back(Word{ s.substr(i, j - i), k < n ? s[k] : '\n' });
                i = j;
                continue;
            }
            if (c != ' ') words.push_back(Word{ std::string(1, c), i + 1 < n ? s[i + 1] : '\n' });
            ++i;
        }
    }

    auto isType = [&](const std::string& w) {
        return IsBuiltinType(w) || scriptTypes_.count(w) ||
               (api_ && (api_->classes.count(w) || api_->aliases.count(w)));
    };
    auto addName = [&](const std::string& name, const std::string& detail, bool callable, int kind) {
        for (const Candidate& c : scriptNames_) if (c.name == name) return;
        scriptNames_.push_back(Candidate{ name, detail, callable, kind });
    };
    // pass 1: declared types (so "Foo f" can be read in pass 2 whatever the order)
    for (size_t i = 0; i + 1 < words.size(); ++i)
        if ((words[i].text == "class" || words[i].text == "enum" || words[i].text == "interface") &&
            IdentStart((unsigned char)words[i + 1].text[0]))
            scriptTypes_.insert(words[i + 1].text);
    // pass 2: functions and typed variables / parameters
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        const std::string& w = words[i].text;
        if ((w == "func" || w == "coroutine") && IdentStart((unsigned char)words[i + 1].text[0])) {
            addName(words[i + 1].text, w == "func" ? "function" : "coroutine", true, 2);
            continue;
        }
        if (!isType(w)) continue;
        size_t j = i + 1;
        if (j < words.size() && words[j].text == "?") ++j;           // Entity? e
        if (j < words.size() && IdentStart((unsigned char)words[j].text[0]) &&
            !IsKeyword(words[j].text) && !isType(words[j].text) && words[j].after != '(') {
            varTypes_[words[j].text] = w;
            addName(words[j].text, w, false, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------
void CodeEditor::Render (const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30, 30, 30, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove);
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    if (indexedVersion_ != version_) Reindex();

    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float charH = ImGui::GetTextLineHeight();
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%d", std::max(999, (int)lines_.size()));
    const float gutter = ImGui::CalcTextSize(digits).x + 22.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();   // scrolled with the content
    const float viewH = window->InnerRect.GetHeight();
    const float viewW = window->InnerRect.GetWidth();

    // content extent: every line, plus room to scroll the last one up a little
    ImGui::Dummy(ImVec2(gutter + maxWidth_ + 64.0f, lines_.size() * lineH + std::max(0.0f, viewH - lineH * 3.0f)));

    const bool hovered = ImGui::IsWindowHovered();
    if (wantFocus_) {
        focused_ = true;
        wantFocus_ = false;
        ImGui::SetWindowFocus();
    }
    // Focus is this widget's own: lost to a click anywhere else, or when another
    //	window takes keyboard focus.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !popupHovered_) focused_ = false;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        focused_ = false;
        completionOpen_ = false;
    }

    HandleMouse(origin, gutter, hovered);
    if (focused_) HandleKeyboard();
    Clamp(cursor_);
    Clamp(anchor_);
    if (indexedVersion_ != version_) Reindex();

    // keep the cursor in view
    if (scrollToCursor_) {
        scrollToCursor_ = false;
        const float cy = cursor_.line * lineH;
        const float sy = ImGui::GetScrollY();
        if (cy < sy) ImGui::SetScrollY(cy);
        else if (cy + lineH > sy + viewH) ImGui::SetScrollY(cy + lineH - viewH);
        const float cx = TextWidth(lines_[cursor_.line], 0, cursor_.col);
        const float sx = ImGui::GetScrollX();
        if (cx < sx) ImGui::SetScrollX(std::max(0.0f, cx - 40.0f));
        else if (cx + gutter + 40.0f > sx + viewW) ImGui::SetScrollX(cx + gutter + 40.0f - viewW);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scrollY = ImGui::GetScrollY();
    const int first = std::max(0, (int)(scrollY / lineH) - 1);
    const int last = std::min((int)lines_.size() - 1, (int)((scrollY + viewH) / lineH) + 1);
    const float textX = origin.x + gutter;
    const Pos selA = SelStart(), selB = SelEnd();

    // the block-comment state has to be carried from the top to the first visible line
    bool inBlock = false;
    std::vector<Span> spans;
    static const std::set<std::string> noTypes;
    std::set<std::string> typesHere = scriptTypes_;
    if (api_) {
        for (const auto& kv : api_->classes) typesHere.insert(kv.first);
        for (const auto& kv : api_->aliases) typesHere.insert(kv.first);
    }
    for (int l = 0; l < first; ++l) Tokenize(lines_[l], inBlock, spans, noTypes, nullptr);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Marker* hoveredMarker = nullptr;

    for (int l = first; l <= last; ++l) {
        const std::string& s = lines_[l];
        const float y = origin.y + l * lineH;
        const float ty = y + (lineH - charH) * 0.5f;

        if (focused_ && l == cursor_.line && !HasSelection())
            dl->AddRectFilled(ImVec2(window->InnerRect.Min.x, y), ImVec2(window->InnerRect.Max.x, y + lineH),
                              IM_COL32(40, 40, 44, 255));
        // selection
        if (HasSelection() && l >= selA.line && l <= selB.line) {
            const int a = l == selA.line ? selA.col : 0;
            const int b = l == selB.line ? selB.col : (int)s.size();
            const float x0 = textX + TextWidth(s, 0, a);
            float x1 = textX + TextWidth(s, 0, b);
            if (l != selB.line) x1 += ImGui::CalcTextSize(" ").x; // the newline is selected too
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x1, x0 + 2.0f), y + lineH), IM_COL32(38, 79, 120, 255));
        }

        // text
        Tokenize(s, inBlock, spans, typesHere, api_);
        float x = textX;
        for (const Span& sp : spans) {
            dl->AddText(ImVec2(x, ty), TokColour(sp.tok), s.c_str() + sp.from, s.c_str() + sp.to);
            x += TextWidth(s, sp.from, sp.to);
        }

        // diagnostics: a squiggle from the column to the end of the word there
        for (const Marker& m : markers_) {
            if (m.line - 1 != l) continue;
            int a = m.column > 0 ? std::min(m.column - 1, (int)s.size()) : LeadingSpaces(s);
            int b = a;
            while (b < (int)s.size() && IdentChar((unsigned char)s[b])) ++b;
            if (b == a) b = std::min((int)s.size(), a + 1);
            if (b == a) { a = 0; b = (int)s.size(); }
            const float x0 = textX + TextWidth(s, 0, a);
            const float x1 = std::max(x0 + 6.0f, textX + TextWidth(s, 0, b));
            const ImU32 col = m.error ? IM_COL32(244, 71, 71, 255) : IM_COL32(205, 173, 0, 255);
            const float base = y + lineH - 2.0f;
            for (float sx = x0; sx < x1; sx += 4.0f) {
                dl->AddLine(ImVec2(sx, base), ImVec2(std::min(sx + 2.0f, x1), base - 2.0f), col, 1.0f);
                dl->AddLine(ImVec2(std::min(sx + 2.0f, x1), base - 2.0f), ImVec2(std::min(sx + 4.0f, x1), base), col, 1.0f);
            }
            if (mouse.y >= y && mouse.y < y + lineH && mouse.x >= x0 - 2 && mouse.x <= x1 + 2) hoveredMarker = &m;
        }

        // cursor
        if (focused_ && l == cursor_.line) {
            const bool on = std::fmod((float)ImGui::GetTime() - blinkStart_, 1.0f) < 0.6f;
            const float cx = textX + TextWidth(s, 0, cursor_.col);
            if (on) dl->AddLine(ImVec2(cx, y + 1), ImVec2(cx, y + lineH - 1), IM_COL32(220, 220, 220, 255), 1.5f);
            // where IME candidate windows open, and the frame's "a text field is active"
            ImGuiContext& g = *ImGui::GetCurrentContext();
            g.PlatformImeData.WantVisible = true;
            g.PlatformImeData.InputPos = ImVec2(cx, y);
            g.PlatformImeData.InputLineHeight = lineH;
        }
    }

    // the gutter, drawn over the text so horizontal scrolling slides under it
    const float gx = window->InnerRect.Min.x;
    dl->AddRectFilled(ImVec2(gx, window->InnerRect.Min.y), ImVec2(gx + gutter - 6.0f, window->InnerRect.Max.y),
                      IM_COL32(30, 30, 30, 255));
    for (int l = first; l <= last; ++l) {
        const float y = origin.y + l * lineH;
        bool err = false, warn = false;
        for (const Marker& m : markers_)
            if (m.line - 1 == l) { (m.error ? err : warn) = true; }
        char num[16];
        std::snprintf(num, sizeof(num), "%d", l + 1);
        const float w = ImGui::CalcTextSize(num).x;
        const ImU32 col = err ? IM_COL32(244, 71, 71, 255)
                        : (l == cursor_.line ? IM_COL32(198, 198, 198, 255) : IM_COL32(110, 118, 129, 255));
        dl->AddText(ImVec2(gx + gutter - 12.0f - w, y + (lineH - charH) * 0.5f), col, num);
        if (err || warn) {
            dl->AddCircleFilled(ImVec2(gx + 6.0f, y + lineH * 0.5f), 3.0f,
                                err ? IM_COL32(244, 71, 71, 255) : IM_COL32(205, 173, 0, 255));
            if (mouse.y >= y && mouse.y < y + lineH && mouse.x >= gx && mouse.x < gx + gutter)
                for (const Marker& m : markers_) if (m.line - 1 == l) { hoveredMarker = &m; break; }
        }
    }
    if (hoveredMarker && hovered) ImGui::SetTooltip("%s", hoveredMarker->message.c_str());

    if (focused_) {
        // tells the app (and ImGui's own shortcuts) that typing belongs to a text field
        ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
        const std::string& s = lines_[cursor_.line];
        DrawCompletion(ImVec2(textX + TextWidth(s, 0, cursor_.col), origin.y + cursor_.line * lineH), lineH);
    } else {
        popupHovered_ = false;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace st::devui
