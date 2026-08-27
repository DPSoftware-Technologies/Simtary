#ifndef SUB_WIN_STATUS_H
#define SUB_WIN_STATUS_H

#include <string>
#include <atomic>
#include <mutex>
#include <thread>

// A small borderless loading window that runs on its OWN thread using the native
// windowing API of each platform — Win32 + GDI on Windows, Xlib on Linux. No SDL.
//
// Why native instead of SDL: SDL's video/windowing/event subsystem is global and
// main-thread-only; driving a second SDL window from a worker thread deadlocks.
// Native windows are per-thread (Win32: each thread owns its message queue; X11:
// the thread owns its own Display connection), so this window keeps painting and
// animating independently while the main thread is blocked initializing the
// engine. Call Show() before the blocking work, Hide() after.
//
// ── Two lines, because two different things report ─────────────────────────────
//
//   status   the PHASE: "Loading materials", "Applying transform". Changes slowly,
//            pushed from the main thread through st::App::SetLoadingStatus.
//   detail   WHAT is being worked on right now: the asset in flight, its size, and
//            how many of how many. Changes constantly, and is pushed from the
//            loading worker threads (st::AssetSystem's progress callback).
//
// They are separate fields rather than one string because they have separate
// producers: with a single field the fast one overwrites the slow one and the phase
// is never readable. The detail line is drawn dimmed and elided in the MIDDLE
// (SS_PATHELLIPSIS on Windows), so "textures/Black_Glass_metallicRoughness.png"
// degrades to "textures\...\Roughness.png" rather than being cut off mid-word.
//
// A new status clears the detail line: a phase change means whatever was in flight
// belonged to the previous phase, and leaving it there reads as a stall.
class SubWinStatus {
public:
    SubWinStatus(const std::string& title, int width, int height);
    ~SubWinStatus();

    // Window title. Only has an effect before Show() — the window is created once, on
    // its own thread, and is not renamed afterwards.
    void SetTitle(const std::string& title);

    void Show();   // spawn the window thread (no-op if already shown)
    void Hide();   // signal stop + join the thread (no-op if not shown)

    // All of these are thread-safe; callable from any thread.
    void SetProgress(int progress);                 // 1..100, or 0 for indeterminate
    void SetStatusText(const std::string& text);    // phase line; clears the detail line
    void SetDetailText(const std::string& text);    // second line, dimmed and elided
    void SetStatus(const std::string& text, const std::string& detail);

private:
    void ThreadMain();   // window + render loop, runs on m_thread

    std::string m_title;
    int m_width;
    int m_height;

    std::thread       m_thread;
    std::atomic<bool> m_running;
    std::atomic<int>  m_progress;

    std::string m_statusText;
    std::string m_detailText;
    std::mutex  m_textMutex;
};

#endif
