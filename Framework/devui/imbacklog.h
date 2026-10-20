#pragma once
// Backlog Viewer - the engine log as a console in the shape of Unity's.
//
//   - one row per entry: level marker, time, a coloured source tag ("[script]",
//     "PlayMode:" - see io/LogFormat.h), the message's first line;
//   - Info / Warning / Error toggles with live counts, text filter, tag filter;
//   - Collapse folds identical entries (same level, same text) into one row with a
//     count, so a script printing the same line every frame takes one row, not the view;
//   - click a row for the full text below, selectable and copyable.
//
// The viewer keeps its own copy of the entries, fed by a wi::backlog listener, so a frame
// costs the visible rows - not re-splitting the whole backlog text as it used to.
//
// No exceptions, no RTTI.

#include <imgui.h>
#include <wiBacklog.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class BacklogViewer {
public:
    BacklogViewer () = default;
    ~BacklogViewer ();
    BacklogViewer (const BacklogViewer&) = delete;
    BacklogViewer& operator= (const BacklogViewer&) = delete;

    void render (bool* isopen);

private:
    struct Row {
        wi::backlog::LogLevel level = wi::backlog::LogLevel::Default;
        double      time = 0;
        std::string tag;
        std::string message;
        uint32_t    group = 0;
    };
    struct Group {
        uint32_t first = 0;   // row index
        uint32_t last  = 0;
        uint32_t count = 0;
    };

    void Connect ();
    void Drain ();
    void Add (const wi::backlog::LogEntry& entry);
    void ClearAll ();
    void Rebuild ();
    bool Passes (const Row& row) const;
    void DrawToolbar ();
    void DrawRows ();
    void DrawDetail ();

    // Filled by the listener on whatever thread posted; drained on the UI thread.
    std::mutex                           pendingLock_;
    std::vector<wi::backlog::LogEntry>   pending_;
    int                                  listener_ = 0;
    uint64_t                             lastSerial_ = 0;

    std::vector<Row>                          rows_;
    std::vector<Group>                        groups_;
    std::unordered_map<std::string, uint32_t> groupOf_;   // level char + text
    std::vector<std::string>                  tags_;      // every tag seen, sorted
    std::vector<std::string>                  hiddenTags_;
    uint32_t                                  counts_[3] = {};

    std::vector<uint32_t> visible_;   // row (or group, collapsed) indices that pass
    bool                  dirty_ = true;
    bool                  scrollToBottom_ = false;

    bool showInfo_ = true;
    bool showWarning_ = true;
    bool showError_ = true;
    bool collapse_ = false;
    bool autoScroll_ = true;
    bool showTime_ = true;
    int  selected_ = -1;      // row index
    ImGuiTextFilter filter_;
};
