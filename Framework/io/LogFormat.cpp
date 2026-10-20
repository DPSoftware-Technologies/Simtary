#include "io/LogFormat.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace st::log {

namespace {

bool StartsWith (const std::string& s, size_t at, const char* prefix) {
    return s.compare(at, std::char_traits<char>::length(prefix), prefix) == 0;
}

bool TagChar (char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-';
}

} // namespace

ParsedEntry Parse (const wi::backlog::LogEntry& entry) {
    const std::string& t = entry.text;
    size_t begin = 0;
    size_t end = t.size();
    while (end > begin && (t[end - 1] == '\n' || t[end - 1] == '\r')) --end;
    if (StartsWith(t, 0, "[Warning] ")) begin = 10;
    else if (StartsWith(t, 0, "[Error] ")) begin = 8;
    // Some engine posts open with a blank line of their own: "\n[wi::initializer] ...".
    while (begin < end && (t[begin] == '\n' || t[begin] == '\r')) ++begin;

    ParsedEntry out;
    if (begin < end && t[begin] == '[') {
        // "[script] ..."
        size_t close = begin + 1;
        while (close < end && close - begin <= 32 && (TagChar(t[close]) || t[close] == ':')) ++close;
        if (close < end && t[close] == ']' && close > begin + 1) {
            out.tag = t.substr(begin + 1, close - begin - 1);
            begin = close + 1;
            while (begin < end && t[begin] == ' ') ++begin;
        }
    } else {
        // "PlayMode: ..."
        size_t colon = begin;
        while (colon < end && colon - begin <= 24 && TagChar(t[colon])) ++colon;
        if (colon > begin && colon + 1 < end && t[colon] == ':' && t[colon + 1] == ' ' &&
            std::isalpha((unsigned char)t[begin])) {
            out.tag = t.substr(begin, colon - begin);
            begin = colon + 2;
        }
    }
    out.message = t.substr(begin, end - begin);
    return out;
}

std::string FormatTime (double unixSeconds, bool millis) {
    const std::time_t secs = (std::time_t)unixSeconds;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[32];
    if (millis) {
        const int ms = (int)((unixSeconds - std::floor(unixSeconds)) * 1000.0);
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return buf;
}

void InstallConsoleLog () {
#ifdef _WIN32
    static bool installed = false;
    if (installed) return;
    installed = true;

    // ANSI colours need virtual-terminal processing; a stdout that is a file or a pipe has
    //	no console mode, and gets the same lines without the escapes.
    bool colour = false;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
        colour = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    SetConsoleOutputCP(CP_UTF8);

    static std::mutex lock;
    wi::backlog::AddListener([colour](const wi::backlog::LogEntry& entry) {
        const ParsedEntry p = Parse(entry);
        if (p.message.empty() && p.tag.empty()) return; // the engine's spacer lines
        const char* levelColour = "";
        const char* levelName = "";
        switch (entry.level) {
        case wi::backlog::LogLevel::Warning: levelColour = "\x1b[93m"; levelName = "WARN  "; break;
        case wi::backlog::LogLevel::Error:   levelColour = "\x1b[91m"; levelName = "ERROR "; break;
        default:                             levelColour = "";         levelName = "";       break;
        }
        std::string line;
        line.reserve(p.message.size() + 48);
        if (colour) line += "\x1b[90m";
        line += FormatTime(entry.time, true);
        line += ' ';
        if (colour) line += "\x1b[0m";
        if (*levelName) {
            if (colour) line += levelColour;
            line += levelName;
        }
        if (!p.tag.empty()) {
            if (colour) line += "\x1b[96m";
            line += '[';
            line += p.tag;
            line += "] ";
            if (colour) line += "\x1b[0m";
        }
        if (colour) line += levelColour;
        line += p.message;
        if (colour) line += "\x1b[0m";
        line += '\n';

        std::scoped_lock lck(lock);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    });
#endif
}

} // namespace st::log
