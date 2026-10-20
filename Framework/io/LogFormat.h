#pragma once
// LogFormat - how a wi::backlog entry is read back into parts, for every place that shows
// the log: the editor's Backlog Viewer and the process console.
//
// An entry's text is "[Warning] " / "[Error] " (added by the engine for those levels) +
// whatever was posted + '\n'. The posted part often starts with a source tag, in one of
// two shapes the codebase already uses:
//
//     [script] TaipanDemo: 5 s of scene time       -> tag "script"
//     PlayMode: snapshot of 812 entities            -> tag "PlayMode"
//
// A colon tag is one word (letters, digits, '_', '.', '-'), so "Dev asset sync: ..." keeps
// its whole text and gets no tag.
//
// InstallConsoleLog() forwards every entry posted from then on to stdout, coloured by
// level, with its time. Windows only: elsewhere the engine already writes the backlog to
// stdout (and stdout to logcat on Android), so a second copy would double every line.
//
// No exceptions, no RTTI.

#include "wiBacklog.h"

#include <string>

namespace st::log {

struct ParsedEntry {
    std::string tag;      // "" when the entry has none
    std::string message;  // level prefix, tag and trailing newline removed
};

ParsedEntry Parse (const wi::backlog::LogEntry& entry);

// "14:03:27" (local time); with millis, "14:03:27.415".
std::string FormatTime (double unixSeconds, bool millis = false);

void InstallConsoleLog ();

} // namespace st::log
