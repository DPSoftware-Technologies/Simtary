#include "imbacklog.h"
#include "io/LogFormat.h"

#include <algorithm>
#include <cstdio>

using wi::backlog::LogLevel;

namespace {

constexpr size_t kMaxRows  = 20000;   // past this the oldest kTrimRows go
constexpr size_t kTrimRows = 5000;

int LevelIndex (LogLevel level) {
    switch (level) {
    case LogLevel::Warning: return 1;
    case LogLevel::Error:   return 2;
    default:                return 0;
    }
}

ImU32 LevelColour (int index) {
    switch (index) {
    case 1:  return IM_COL32(255, 199, 72, 255);
    case 2:  return IM_COL32(255, 107, 102, 255);
    default: return IM_COL32(205, 209, 216, 255);
    }
}

// A stable colour per tag, so "script" is the same cyan in every session.
ImU32 TagColour (const std::string& tag) {
    uint32_t h = 2166136261u;
    for (char c : tag) h = (h ^ (uint8_t)c) * 16777619u;
    const float hue = (float)(h % 360u) / 360.0f;
    return (ImU32)ImColor::HSV(hue, 0.50f, 0.98f);
}

ImU32 WithAlpha (ImU32 colour, int alpha) {
    return (colour & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// Counted on/off button for the level toggles: lit in the level's colour while on.
bool LevelToggle (const char* label, uint32_t count, bool& on, int level) {
    char text[64];
    std::snprintf(text, sizeof(text), "%s %u##%s", label, count, label);
    const ImU32 c = LevelColour(level);
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(c, 60));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(c, 90));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(c, 120));
        ImGui::PushStyleColor(ImGuiCol_Text, c);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    const bool pressed = ImGui::SmallButton(text);
    ImGui::PopStyleColor(4);
    if (pressed) on = !on;
    return pressed;
}

const char* FirstLineEnd (const std::string& s) {
    const size_t nl = s.find('\n');
    return s.c_str() + (nl == std::string::npos ? s.size() : nl);
}

} // namespace

BacklogViewer::~BacklogViewer () {
    if (listener_ != 0) wi::backlog::RemoveListener(listener_);
}

void BacklogViewer::Connect () {
    // Listener first, then the history: an entry posted in between lands in both, and the
    //	serial check drops the second copy.
    listener_ = wi::backlog::AddListener([this](const wi::backlog::LogEntry& entry) {
        std::scoped_lock lck(pendingLock_);
        pending_.push_back(entry);
    });
    std::vector<wi::backlog::LogEntry> existing;
    wi::backlog::GetEntries(existing);
    for (const auto& e : existing) {
        if (e.serial <= lastSerial_) continue;
        Add(e);
        lastSerial_ = e.serial;
    }
    scrollToBottom_ = true;
}

void BacklogViewer::Drain () {
    std::vector<wi::backlog::LogEntry> batch;
    {
        std::scoped_lock lck(pendingLock_);
        batch.swap(pending_);
    }
    if (batch.empty()) return;
    // Posts from several threads can reach the queue out of serial order.
    std::sort(batch.begin(), batch.end(),
              [](const auto& a, const auto& b) { return a.serial < b.serial; });
    for (const auto& e : batch) {
        if (e.serial <= lastSerial_) continue;
        Add(e);
        lastSerial_ = e.serial;
    }
    scrollToBottom_ = true;
}

void BacklogViewer::Add (const wi::backlog::LogEntry& entry) {
    if (rows_.size() >= kMaxRows) {
        // Drop the oldest block and re-fold what is left; cheaper than a ring per frame.
        std::vector<Row> keep(rows_.begin() + kTrimRows, rows_.end());
        selected_ = selected_ >= (int)kTrimRows ? selected_ - (int)kTrimRows : -1;
        rows_.clear();
        groups_.clear();
        groupOf_.clear();
        counts_[0] = counts_[1] = counts_[2] = 0;
        for (Row& r : keep) {
            wi::backlog::LogEntry e;
            e.level = r.level;
            e.time = r.time;
            e.text = r.tag.empty() ? r.message : "[" + r.tag + "] " + r.message;
            Add(e);
        }
    }

    const st::log::ParsedEntry p = st::log::Parse(entry);
    if (p.message.empty() && p.tag.empty()) return; // the engine's spacer lines
    Row row;
    row.level = entry.level;
    row.time = entry.time;
    row.tag = p.tag;
    row.message = p.message;

    const int li = LevelIndex(entry.level);
    ++counts_[li];
    std::string key;
    key.reserve(row.tag.size() + row.message.size() + 2);
    key += (char)('0' + li);
    key += row.tag;
    key += '\x1f';
    key += row.message;
    const uint32_t index = (uint32_t)rows_.size();
    auto it = groupOf_.find(key);
    if (it == groupOf_.end()) {
        it = groupOf_.emplace(std::move(key), (uint32_t)groups_.size()).first;
        groups_.push_back(Group{ index, index, 0 });
    }
    Group& g = groups_[it->second];
    g.last = index;
    ++g.count;
    row.group = it->second;

    if (!row.tag.empty()) {
        auto at = std::lower_bound(tags_.begin(), tags_.end(), row.tag);
        if (at == tags_.end() || *at != row.tag) tags_.insert(at, row.tag);
    }
    rows_.push_back(std::move(row));
    dirty_ = true;
}

void BacklogViewer::ClearAll () {
    rows_.clear();
    groups_.clear();
    groupOf_.clear();
    counts_[0] = counts_[1] = counts_[2] = 0;
    visible_.clear();
    selected_ = -1;
    dirty_ = true;
}

bool BacklogViewer::Passes (const Row& row) const {
    switch (LevelIndex(row.level)) {
    case 1:  if (!showWarning_) return false; break;
    case 2:  if (!showError_) return false; break;
    default: if (!showInfo_) return false; break;
    }
    if (!row.tag.empty() &&
        std::find(hiddenTags_.begin(), hiddenTags_.end(), row.tag) != hiddenTags_.end())
        return false;
    if (!filter_.IsActive()) return true;
    return filter_.PassFilter(row.message.c_str()) ||
           (!row.tag.empty() && filter_.PassFilter(row.tag.c_str()));
}

void BacklogViewer::Rebuild () {
    visible_.clear();
    if (collapse_) {
        for (uint32_t g = 0; g < (uint32_t)groups_.size(); ++g)
            if (Passes(rows_[groups_[g].first])) visible_.push_back(g);
    } else {
        for (uint32_t r = 0; r < (uint32_t)rows_.size(); ++r)
            if (Passes(rows_[r])) visible_.push_back(r);
    }
    dirty_ = false;
}

void BacklogViewer::DrawToolbar () {
    if (ImGui::SmallButton("Clear")) {
        wi::backlog::clear();
        ClearAll();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Collapse", &collapse_)) dirty_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fold identical messages into one row with a count");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Time", &showTime_);

    ImGui::SameLine();
    const bool anyHidden = !hiddenTags_.empty();
    if (anyHidden) ImGui::PushStyleColor(ImGuiCol_Text, LevelColour(1));
    if (ImGui::SmallButton(anyHidden ? "Tags*" : "Tags")) ImGui::OpenPopup("##backlog_tags");
    if (anyHidden) ImGui::PopStyleColor();
    if (ImGui::BeginPopup("##backlog_tags")) {
        if (ImGui::SmallButton("All")) { hiddenTags_.clear(); dirty_ = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) { hiddenTags_ = tags_; dirty_ = true; }
        ImGui::Separator();
        if (tags_.empty()) ImGui::TextDisabled("no tagged messages yet");
        for (const std::string& tag : tags_) {
            auto at = std::find(hiddenTags_.begin(), hiddenTags_.end(), tag);
            bool shown = at == hiddenTags_.end();
            ImGui::PushStyleColor(ImGuiCol_Text, TagColour(tag));
            if (ImGui::Checkbox(tag.c_str(), &shown)) {
                if (shown) hiddenTags_.erase(at);
                else hiddenTags_.push_back(tag);
                dirty_ = true;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    // Level toggles at the right end, filter takes what is left between.
    const ImGuiStyle& style = ImGui::GetStyle();
    char buf[64];
    float togglesWidth = 0;
    const char* names[3] = { "Info", "Warn", "Error" };
    for (int i = 0; i < 3; ++i) {
        std::snprintf(buf, sizeof(buf), "%s %u", names[i], counts_[i]);
        togglesWidth += ImGui::CalcTextSize(buf).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    }
    ImGui::SameLine();
    const float filterWidth = ImGui::GetContentRegionAvail().x - togglesWidth;
    ImGui::SetNextItemWidth(std::max(80.0f, filterWidth));
    if (ImGui::InputTextWithHint("##backlog_filter", "Filter  (word, -exclude, a,b)",
                                 filter_.InputBuf, IM_ARRAYSIZE(filter_.InputBuf))) {
        filter_.Build();
        dirty_ = true;
    }
    ImGui::SameLine();
    if (LevelToggle(names[0], counts_[0], showInfo_, 0)) dirty_ = true;
    ImGui::SameLine();
    if (LevelToggle(names[1], counts_[1], showWarning_, 1)) dirty_ = true;
    ImGui::SameLine();
    if (LevelToggle(names[2], counts_[2], showError_, 2)) dirty_ = true;
}

void BacklogViewer::DrawRows () {
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float timeW = ImGui::CalcTextSize("00:00:00").x;

    ImGuiListClipper clipper;
    clipper.Begin((int)visible_.size(), lineH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const uint32_t v = visible_[i];
            const Group* group = collapse_ ? &groups_[v] : nullptr;
            const uint32_t rowIndex = group ? group->last : v;
            const Row& row = rows_[rowIndex];
            const int li = LevelIndex(row.level);
            const bool isSelected = selected_ >= 0 && (group ? rows_[selected_].group == row.group
                                                             : selected_ == (int)rowIndex);

            ImGui::PushID(i);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_None,
                                  ImVec2(0, lineH - ImGui::GetStyle().ItemSpacing.y)))
                selected_ = (int)rowIndex;
            if (ImGui::BeginPopupContextItem("##row_menu")) {
                selected_ = (int)rowIndex;
                if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(row.message.c_str());
                if (ImGui::MenuItem("Copy line")) {
                    std::string line = st::log::FormatTime(row.time, true) + " ";
                    if (!row.tag.empty()) line += "[" + row.tag + "] ";
                    line += row.message;
                    ImGui::SetClipboardText(line.c_str());
                }
                if (!row.tag.empty()) {
                    ImGui::Separator();
                    const std::string hide = "Hide [" + row.tag + "]";
                    if (ImGui::MenuItem(hide.c_str())) {
                        hiddenTags_.push_back(row.tag);
                        dirty_ = true;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + lineH);
            if (!isSelected && (i & 1))
                dl->AddRectFilled(min, ImVec2(max.x, max.y - ImGui::GetStyle().ItemSpacing.y),
                                  IM_COL32(255, 255, 255, 8));
            if (li > 0) // tint the whole row for warnings and errors
                dl->AddRectFilled(min, ImVec2(min.x + 3, max.y - ImGui::GetStyle().ItemSpacing.y),
                                  LevelColour(li));

            const float textY = min.y;
            float x = min.x + 10;
            dl->AddCircleFilled(ImVec2(x, textY + ImGui::GetTextLineHeight() * 0.5f), 3.5f, LevelColour(li));
            x += 10;

            if (showTime_) {
                const std::string t = st::log::FormatTime(row.time);
                dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), t.c_str());
                x += timeW + 8;
            }

            // Count badge, right aligned; the message is clipped short of it.
            float right = max.x - 4;
            if (group && group->count > 1) {
                char n[16];
                std::snprintf(n, sizeof(n), "%u", group->count);
                const ImVec2 ts = ImGui::CalcTextSize(n);
                const ImVec2 b0(right - ts.x - 10, textY);
                const ImVec2 b1(right, textY + ImGui::GetTextLineHeight());
                dl->AddRectFilled(b0, b1, WithAlpha(LevelColour(li), 70), ImGui::GetTextLineHeight() * 0.5f);
                dl->AddText(ImVec2(b0.x + 5, textY), LevelColour(li), n);
                right = b0.x - 6;
            }

            if (!row.tag.empty()) {
                const ImU32 tc = TagColour(row.tag);
                const ImVec2 ts = ImGui::CalcTextSize(row.tag.c_str());
                dl->AddRectFilled(ImVec2(x - 3, textY), ImVec2(x + ts.x + 3, textY + ts.y),
                                  WithAlpha(tc, 40), 3.0f);
                dl->AddText(ImVec2(x, textY), tc, row.tag.c_str());
                x += ts.x + 10;
            }

            const char* end = FirstLineEnd(row.message);
            const ImVec4 clip(x, min.y, std::max(x, right), max.y);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, textY), LevelColour(li),
                        row.message.c_str(), end, 0.0f, &clip);
            if (*end == '\n') { // more lines in the detail pane
                const float w = ImGui::CalcTextSize(row.message.c_str(), end).x;
                if (x + w + 30 < right)
                    dl->AddText(ImVec2(x + w + 8, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
            }
        }
    }

    if (autoScroll_ && scrollToBottom_ && atBottom) ImGui::SetScrollHereY(1.0f);
    scrollToBottom_ = false;
}

void BacklogViewer::DrawDetail () {
    if (selected_ < 0 || selected_ >= (int)rows_.size()) {
        ImGui::TextDisabled("Click a message to see all of it here.");
        return;
    }
    const Row& row = rows_[selected_];
    const int li = LevelIndex(row.level);
    const char* levelName = li == 2 ? "Error" : li == 1 ? "Warning" : "Info";
    ImGui::TextColored(ImColor(LevelColour(li)), "%s", levelName);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", st::log::FormatTime(row.time, true).c_str());
    if (!row.tag.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(TagColour(row.tag)), "[%s]", row.tag.c_str());
    }
    const uint32_t count = groups_[row.group].count;
    if (count > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("x%u", count);
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 40);
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(row.message.c_str());

    // Read-only, but selectable - the reason it is an input and not plain text.
    std::string text = row.message;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::InputTextMultiline("##backlog_detail", &text[0], text.size() + 1, ImVec2(-1, -1),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
}

void BacklogViewer::render (bool* isopen) {
    if (listener_ == 0) Connect();
    Drain();

    ImGui::SetNextWindowSize(ImVec2(820, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Backlog Viewer", isopen)) {
        ImGui::End();
        return;
    }

    DrawToolbar();
    if (dirty_) Rebuild();
    ImGui::Separator();

    const float detailH = 110.0f;
    ImGui::BeginChild("##backlog_rows", ImVec2(0, -detailH), false);
    DrawRows();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("##backlog_detail_pane", ImVec2(0, 0), false);
    DrawDetail();
    ImGui::EndChild();

    ImGui::End();
}
