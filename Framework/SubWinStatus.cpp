#include "SubWinStatus.h"

SubWinStatus::SubWinStatus(const std::string& title, int width, int height)
    : m_title(title), m_width(width), m_height(height),
      m_running(false), m_progress(0), m_statusText("Loading...") {}

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
}

// ============================================================================
//  Windows — real Win32 dialog using native Common Controls v6 (themed look)
// ============================================================================
#if defined(_WIN32)

#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
// Pull in Common Controls v6 so the progress bar is the themed (Aero) style
// instead of the legacy gray one. One string literal — do not wrap.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static LRESULT CALLBACK SubWinProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        // Paint the label text transparently over the dialog face color.
        SetBkMode((HDC)w, TRANSPARENT);
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

    HWND hwnd = CreateWindowExA(exStyle, CLS, m_title.c_str(), style,
        x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { m_running = false; return; }

    // System message-box font, so text matches native dialogs.
    NONCLIENTMETRICSA ncm = { sizeof(ncm) };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirectA(&ncm.lfMessageFont);

    const int pad = 15;
    HWND label = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, m_height / 2 - 35, m_width - pad * 2, 22, hwnd, nullptr, hInst, nullptr);
    HWND prog = CreateWindowExA(0, PROGRESS_CLASSA, "",
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        pad, m_height / 2 - 5, m_width - pad * 2, 20, hwnd, nullptr, hInst, nullptr);

    SendMessageA(label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(prog,  PBM_SETMARQUEE, TRUE, 30);   // animate the marquee

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    std::string lastText;
    bool marquee = true;

    while (m_running.load()) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Status text (only push to the control when it changes).
        std::string text;
        { std::lock_guard<std::mutex> lock(m_textMutex); text = m_statusText; }
        if (text != lastText) {
            SetWindowTextA(label, text.c_str());
            lastText = text;
        }

        // Progress: 0 -> marquee (indeterminate), >0 -> determinate fill.
        const int p = m_progress.load();
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
//  Linux — Xlib, double-buffered with a backing Pixmap
//  (True system-native widgets on Linux would need GTK/Qt; this stays
//   dependency-light and matches the dark loading style.)
// ============================================================================
#else

#include <X11/Xlib.h>
#include <unistd.h>

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

        // Status text (centered, upper half)
        std::string text;
        { std::lock_guard<std::mutex> lock(m_textMutex); text = m_statusText; }
        XSetForeground(dpy, gc, colText);
        if (font) {
            const int tw = XTextWidth(font, text.c_str(), (int)text.size());
            const int tx = (m_width - tw) / 2;
            const int ty = m_height / 2 - 20;
            XDrawString(dpy, buf, gc, tx, ty, text.c_str(), (int)text.size());
        }

        // Progress bar frame
        const int bx = 20, by = m_height / 2 + 10, bw = m_width - 40, bh = 20;
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
