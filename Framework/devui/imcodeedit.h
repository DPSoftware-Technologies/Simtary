#pragma once
// CodeEditor - a source-code text widget for ImGui, built for Taipan scripts.
//
// ImGui's InputTextMultiline draws every character in one colour and owns its keys, so it
// can neither highlight nor run a completion popup. This widget does its own text, cursor
// and selection, and gets both:
//
//	highlighting   keywords, types, strings / f-strings, numbers, comments, calls,
//	               namespaces and classes, each in its own colour
//	completion     keywords, the script's own names, and the VM's whole native API
//	               (math.*, scene.find, Entity's members, ...) - after "x." it lists the
//	               members of whatever x is: a namespace, or a variable declared with a
//	               known class ("Entity self", "Entity? e = ...")
//	diagnostics    markers from the type checker: a squiggle under the spot, a dot in the
//	               gutter, the message on hover
//	editing        undo/redo, clipboard, word moves, auto-indent, bracket pairs,
//	               Ctrl+/ toggles comments, Tab / Shift+Tab indent a selection
//
// Keys while focused: Ctrl+Space opens completion; Up/Down/Enter/Tab/Esc drive it.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

namespace st::devui {

class CodeEditor {
public:
    struct Marker {
        int         line = 0;     // 1-based
        int         column = 0;   // 1-based; 0 = whole line
        bool        error = true;
        std::string message;
    };

    // What completion knows beyond the text itself. Filled by the owner (the Script
    //	Editor builds it from st::taipan::ListSymbols).
    struct Symbol {
        std::string name;
        std::string detail;      // signature or type, shown dimmed
        bool        callable = false;
    };
    struct Api {
        std::vector<Symbol>                        globals;    // print, wait, range, ...
        std::map<std::string, std::vector<Symbol>> namespaces; // "math" -> sin, cos, ...
        std::map<std::string, std::vector<Symbol>> classes;    // "Entity" -> x, translate, ...
        std::map<std::string, std::string>         aliases;    // "entity" -> "Entity"
    };

    void        SetText (const std::string& text);
    std::string GetText () const;
    // Bumped by every edit; compare to notice changes.
    uint64_t    Version () const { return version_; }

    void SetMarkers (std::vector<Marker> markers) { markers_ = std::move(markers); }
    void SetApi (const Api* api) { api_ = api; }

    // Put the cursor at a 1-based line / column, scroll to it, take focus.
    void JumpTo (int line, int column);
    void Focus () { wantFocus_ = true; }
    bool Focused () const { return focused_; }
    int  CursorLine () const { return cursor_.line + 1; }
    int  CursorColumn () const;

    // Draws the widget filling `size` (0 = remaining space).
    void Render (const char* id, ImVec2 size = ImVec2(0, 0));

private:
    struct Pos {
        int line = 0;
        int col = 0;   // byte offset in the line
        bool operator== (const Pos& o) const { return line == o.line && col == o.col; }
        bool operator!= (const Pos& o) const { return !(*this == o); }
        bool operator< (const Pos& o) const { return line < o.line || (line == o.line && col < o.col); }
    };
    struct Snapshot {
        std::vector<std::string> lines;
        Pos cursor, anchor;
    };
    struct Candidate {
        std::string name, detail;
        bool        callable = false;
        int         kind = 0;   // colour: 0 word, 1 keyword, 2 function, 3 type, 4 property
    };

    // editing primitives
    bool HasSelection () const { return cursor_ != anchor_; }
    Pos  SelStart () const { return cursor_ < anchor_ ? cursor_ : anchor_; }
    Pos  SelEnd () const { return cursor_ < anchor_ ? anchor_ : cursor_; }
    std::string SelectedText () const;
    void DeleteSelection ();
    void Insert (const std::string& text);
    void PushUndo (bool mergeTyping);
    void Undo ();
    void Redo ();
    void Edited ();
    void Clamp (Pos& p) const;

    // cursor movement
    Pos  Left (Pos p, bool word) const;
    Pos  Right (Pos p, bool word) const;
    void MoveTo (Pos p, bool select);
    void SelectWordAt (Pos p);

    // features
    void HandleKeyboard ();
    void HandleMouse (const ImVec2& origin, float gutter, bool hovered);
    void ToggleComment ();
    void Indent (bool outdent);
    void NewLine ();

    // completion
    void UpdateCompletion (bool force);
    void AcceptCompletion ();
    void DrawCompletion (const ImVec2& cursorScreen, float lineHeight);
    std::string WordBeforeCursor (std::string* owner) const;
    std::string TypeOfVariable (const std::string& name) const;

    // layout helpers
    float TextWidth (const std::string& line, int fromCol, int toCol) const;
    int   ColumnAt (int line, float x) const;
    // Re-scan the text for its own names (functions, classes, typed variables). Once per
    //	edit, not per frame.
    void Reindex ();

    // what the text itself declares (Reindex)
    uint64_t indexedVersion_ = 0;
    std::set<std::string> scriptTypes_;              // class / enum / interface names
    std::vector<Candidate> scriptNames_;             // functions, globals, locals, params
    std::map<std::string, std::string> varTypes_;    // "self" -> "Entity"
    float maxWidth_ = 0.0f;                          // widest line, for the h-scrollbar
    ImVec2 popupMin_, popupMax_;                     // completion box, last frame
    bool popupHovered_ = false;

    std::vector<std::string> lines_ = { std::string() };
    Pos cursor_, anchor_;
    float preferredX_ = -1.0f;     // keeps the column when moving up / down
    uint64_t version_ = 1;

    std::vector<Snapshot> undo_, redo_;
    double lastTypingTime_ = 0.0;
    bool   lastWasTyping_ = false;

    std::vector<Marker> markers_;
    const Api* api_ = nullptr;

    bool focused_ = false;
    bool wantFocus_ = false;
    bool scrollToCursor_ = false;
    bool dragging_ = false;
    float blinkStart_ = 0.0f;

    bool completionOpen_ = false;
    std::vector<Candidate> candidates_;
    int  completionIndex_ = 0;
    int  completionScroll_ = 0;
    int  completionPrefixLen_ = 0;
};

} // namespace st::devui
