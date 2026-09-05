#include "SubWinStatus.h"

SubWinStatus::SubWinStatus(const std::string& title, int width, int height)
    : m_title(title), m_width(width), m_height(height),
      m_running(false), m_progress(0), m_statusText("Loading...") {}

void SubWinStatus::SetTitle(const std::string& title) {
    // Read once, by the window thread, at CreateWindow time. Setting it after Show()
    // is a no-op rather than a cross-thread rename, which would need its own message.
    std::lock_guard<std::mutex> lock(m_textMutex);
    m_title = title;
}

SubWinStatus::~SubWinStatus() {
    Hide();
}

void SubWinStatus::Show() {
    if (m_running.exchange(true)) return;   // already running
    m_thread = std::thread(&SubWinStatus::ThreadMain, this);
}

void SubWinStatus::Hide() {
    if (!m_running.exchange(false)) return; // not running
    if (m_thread.joinable())
        m_thread.join();
}

void SubWinStatus::SetProgress(int progress) {
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    m_progress.store(progress);
}

void SubWinStatus::SetStatusText(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_textMutex);
    m_statusText = text;
    // A phase change retires whatever was in flight during the previous phase. Leaving
    // a stale asset name under a new heading reads as a stall on that asset.
    m_detailText.clear();
}

void SubWinStatus::SetDetailText(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_textMutex);
    m_detailText = text;
}

void SubWinStatus::SetStatus(const std::string& text, const std::string& detail) {
    std::lock_guard<std::mutex> lock(m_textMutex);
    m_statusText = text;
    m_detailText = detail;
}

// ============================================================================
//  Windows - real Win32 dialog using native Common Controls v6 (themed look)
// ============================================================================
#if defined(_WIN32)

#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
// Pull in Common Controls v6 so the progress bar is the themed (Aero) style
// instead of the legacy gray one. One string literal - do not wrap.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Control IDs, so WM_CTLCOLORSTATIC can tell the three labels apart without keeping
// their handles anywhere the window procedure can reach.
enum : int { ID_STATUS = 101, ID_DETAIL = 102, ID_PERCENT = 103, ID_PROGRESS = 104 };

static LRESULT CALLBACK SubWinProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        // Paint the label text transparently over the dialog face color.
        SetBkMode((HDC)w, TRANSPARENT);
        const int id = GetDlgCtrlID((HWND)l);
        if (id == ID_DETAIL || id == ID_PERCENT) {
            // The secondary lines are dimmed so the eye lands on the phase first. The
            // system grey rather than a literal colour, so this still reads on a
            // high-contrast theme.
            SetTextColor((HDC)w, GetSysColor(COLOR_GRAYTEXT));
        }
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_CLOSE:
        return 0; // closed programmatically by Hide(); ignore the [X]
    }
    return DefWindowProcA(h, msg, w, l);
}

void SubWinStatus::ThreadMain() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const char* CLS = "MilistrySubWinStatus";
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = SubWinProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);  // standard dialog background
    wc.lpszClassName = CLS;
    RegisterClassA(&wc);  // harmless if a previous Show() already registered it

    // A fixed (non-resizable) titled window, sized so the CLIENT area is WxH.
    const DWORD style   = WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    RECT rc = { 0, 0, m_width, m_height };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    std::string title;
    { std::lock_guard<std::mutex> lock(m_textMutex); title = m_title; }

    HWND hwnd = CreateWindowExA(exStyle, CLS, title.c_str(), style,
        x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { m_running = false; return; }

    // System message-box font, so text matches native dialogs.
    NONCLIENTMETRICSA ncm = { sizeof(ncm) };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirectA(&ncm.lfMessageFont);

    // Fixed rows, not offsets from m_height/2. The old layout put the label at
    // height/2-35 and the bar at height/2-5, which for any height under about 90
    // overlapped them and clipped the label off the top.
    const int pad        = 16;
    const int lineHeight = 18;
    const int percentW   = 44;
    const int innerW     = m_width - pad * 2;

    const int statusY   = pad;
    const int detailY   = statusY + lineHeight + 4;
    const int progressY = detailY + lineHeight + 10;

    // SS_ENDELLIPSIS on the phase, SS_PATHELLIPSIS on the detail. The detail line is a
    // path, and dropping its MIDDLE keeps both the folder and the file name readable,
    // where dropping the tail leaves every texture in a folder looking identical.
    // SS_NOPREFIX because '&' is legal in an asset path and would otherwise be eaten as
    // a mnemonic underscore.
    HWND label = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
        pad, statusY, innerW - percentW, lineHeight,
        hwnd, (HMENU)(INT_PTR)ID_STATUS, hInst, nullptr);
    HWND percent = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX,
        pad + innerW - percentW, statusY, percentW, lineHeight,
        hwnd, (HMENU)(INT_PTR)ID_PERCENT, hInst, nullptr);
    HWND detail = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX,
        pad, detailY, innerW, lineHeight,
        hwnd, (HMENU)(INT_PTR)ID_DETAIL, hInst, nullptr);
    HWND prog = CreateWindowExA(0, PROGRESS_CLASSA, "",
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        pad, progressY, innerW, 16,
        hwnd, (HMENU)(INT_PTR)ID_PROGRESS, hInst, nullptr);

    SendMessageA(label,   WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(detail,  WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(percent, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(prog,  PBM_SETMARQUEE, TRUE, 30);   // animate the marquee

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    std::string lastText, lastDetail, lastPercent;
    bool marquee = true;

    while (m_running.load()) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Both lines, pushed only when they change: SetWindowText invalidates and
        // repaints, and the detail line changes on every asset read.
        std::string text, detailText;
        {
            std::lock_guard<std::mutex> lock(m_textMutex);
            text       = m_statusText;
            detailText = m_detailText;
        }
        if (text != lastText) {
            SetWindowTextA(label, text.c_str());
            lastText = text;
        }
        if (detailText != lastDetail) {
            SetWindowTextA(detail, detailText.c_str());
            lastDetail = detailText;
        }

        // Progress: 0 -> marquee (indeterminate), >0 -> determinate fill.
        const int p = m_progress.load();
        const std::string percentText = (p > 0) ? (std::to_string(p) + "%") : std::string();
        if (percentText != lastPercent) {
            SetWindowTextA(percent, percentText.c_str());
            lastPercent = percentText;
        }
        if (p == 0) {
            if (!marquee) {
                SetWindowLongPtrA(prog, GWL_STYLE,
                    GetWindowLongPtrA(prog, GWL_STYLE) | PBS_MARQUEE);
                SendMessageA(prog, PBM_SETMARQUEE, TRUE, 30);
                marquee = true;
            }
        } else {
            if (marquee) {
                SendMessageA(prog, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrA(prog, GWL_STYLE,
                    GetWindowLongPtrA(prog, GWL_STYLE) & ~PBS_MARQUEE);
                SendMessageA(prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
                marquee = false;
            }
            SendMessageA(prog, PBM_SETPOS, p, 0);
        }

        Sleep(16);
    }

    DestroyWindow(hwnd);
    if (font) DeleteObject(font);
    UnregisterClassA(CLS, hInst);
}

// ============================================================================
//  Linux - Xlib, double-buffered with a backing Pixmap
//  (True system-native widgets on Linux would need GTK/Qt; this stays
//   dependency-light and matches the dark loading style.)
// ============================================================================
#else

#include <X11/Xlib.h>
#include <unistd.h>

namespace {
// X11 has no equivalent of SS_PATHELLIPSIS, so the middle of an over-long path is
// dropped by hand. Keeping the head and the tail matters here for the same reason it
// does on Windows: the tail alone makes every texture in a folder look the same, and
// the head alone tells you nothing about which one it is.
std::string ElideMiddle(XFontStruct* font, const std::string& text, int maxWidth) {
    if (font == nullptr || text.empty()) return text;
    if (XTextWidth(font, text.c_str(), (int)text.size()) <= maxWidth) return text;

    const std::string gap = "...";
    // Shrink symmetrically from the middle until it fits. Linear rather than a binary
    // search on purpose: this runs once per frame on a string of a few dozen
    // characters, and the simple version is the one that is obviously correct.
    for (size_t drop = 1; drop + 4 < text.size(); ++drop) {
        const size_t keepFront = (text.size() - drop) / 2;
        const size_t keepBack  = text.size() - drop - keepFront;
        const std::string candidate = text.substr(0, keepFront) + gap +
                                      text.substr(text.size() - keepBack);
        if (XTextWidth(font, candidate.c_str(), (int)candidate.size()) <= maxWidth)
            return candidate;
    }
    return gap;
}
} // namespace

void SubWinStatus::ThreadMain() {
    Display* dpy = XOpenDisplay(nullptr);    // this thread owns this connection
    if (!dpy) { m_running = false; return; }

    const int    screen = DefaultScreen(dpy);
    const int    depth  = DefaultDepth(dpy, screen);
    const Window root   = RootWindow(dpy, screen);
    const int    sw     = DisplayWidth(dpy, screen);
    const int    sh     = DisplayHeight(dpy, screen);
    const int    x      = (sw - m_width) / 2;
    const int    y      = (sh - m_height) / 2;

    const unsigned long colBg     = 0x1E1E1E; // 30,30,30
    const unsigned long colText   = 0xE6E6E6;
    const unsigned long colDim    = 0x969696; // 150,150,150 - the detail line
    const unsigned long colBorder = 0x505050; // 80,80,80
    const unsigned long colBlue   = 0x0096FF; // 0,150,255
    const unsigned long colGreen  = 0x00C864; // 0,200,100

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;          // borderless, no WM decorations
    attrs.background_pixel  = colBg;
    attrs.event_mask        = ExposureMask;
    Window win = XCreateWindow(
        dpy, root, x, y, m_width, m_height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    XMapRaised(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XFontStruct* font = XLoadQueryFont(dpy, "9x15bold");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    Pixmap buf = XCreatePixmap(dpy, win, m_width, m_height, depth);

    float pos = 0.0f, speed = 0.02f;

    while (m_running.load()) {
        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }

        // Background
        XSetForeground(dpy, gc, colBg);
        XFillRectangle(dpy, buf, gc, 0, 0, m_width, m_height);

        // Two left-aligned lines on fixed rows: the phase, then what is in flight.
        // Left-aligned rather than centred because the detail line changes on every
        // asset and a centred line that re-centres constantly is unreadable.
        std::string text, detailText;
        {
            std::lock_guard<std::mutex> lock(m_textMutex);
            text       = m_statusText;
            detailText = m_detailText;
        }

        const int pad     = 16;
        const int innerW  = m_width - pad * 2;
        const int lineH   = 18;
        const int statusY = pad + 12;
        const int detailY = statusY + lineH;

        if (font) {
            const std::string line = ElideMiddle(font, text, innerW);
            XSetForeground(dpy, gc, colText);
            XDrawString(dpy, buf, gc, pad, statusY, line.c_str(), (int)line.size());

            if (!detailText.empty()) {
                const std::string detail = ElideMiddle(font, detailText, innerW);
                XSetForeground(dpy, gc, colDim);
                XDrawString(dpy, buf, gc, pad, detailY, detail.c_str(), (int)detail.size());
            }
        }

        // Progress bar frame
        const int bx = pad, by = detailY + 14, bw = innerW, bh = 16;
        XSetForeground(dpy, gc, colBorder);
        XDrawRectangle(dpy, buf, gc, bx, by, bw, bh);

        const int prog = m_progress.load();
        if (prog == 0) {
            pos += speed;
            if (pos > 1.0f || pos < 0.0f) speed = -speed;
            const int cw   = (bw - 4) / 4;
            const int minX = bx + 2;
            const int maxX = bx + bw - 2 - cw;
            const int cx   = minX + (int)((maxX - minX) * pos);
            XSetForeground(dpy, gc, colBlue);
            XFillRectangle(dpy, buf, gc, cx, by + 2, cw, bh - 3);
        } else {
            const float pct = prog / 100.0f;
            XSetForeground(dpy, gc, colGreen);
            XFillRectangle(dpy, buf, gc, bx + 2, by + 2, (int)((bw - 4) * pct), bh - 3);
        }

        XCopyArea(dpy, buf, win, gc, 0, 0, m_width, m_height, 0, 0);
        XFlush(dpy);
        usleep(16000);
    }

    if (font) XFreeFont(dpy, font);
    XFreePixmap(dpy, buf);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}

#endif
