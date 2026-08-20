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
class SubWinStatus {
public:
    SubWinStatus(const std::string& title, int width, int height);
    ~SubWinStatus();

    void Show();   // spawn the window thread (no-op if already shown)
    void Hide();   // signal stop + join the thread (no-op if not shown)

    // Thread-safe; callable from any thread.
    void SetProgress(int progress);          // 1..100, or 0 for indeterminate
    void SetStatusText(const std::string& text);

private:
    void ThreadMain();   // window + render loop, runs on m_thread

    std::string m_title;
    int m_width;
    int m_height;

    std::thread       m_thread;
    std::atomic<bool> m_running;
    std::atomic<int>  m_progress;

    std::string m_statusText;
    std::mutex  m_textMutex;
};

#endif
