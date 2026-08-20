#ifndef GFX_H
#define GFX_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

#ifdef GFXSDL
// Libraries must not hijack the user's main() entry point.
// SDL_MAIN_HANDLED suppresses SDL's "#define main SDL_main" macro so that
// callers keep their own main(). The library calls SDL_SetMainReady() in its
// constructor before SDL_Init(), which is the required companion call.
#ifndef SDL_MAIN_HANDLED
#  define SDL_MAIN_HANDLED
#endif
#include <SDL2/SDL.h>
#endif

// ===== COLOR FORMAT: ARGB8888 ================================================
//
// All color values are 32-bit ARGB8888:
//
//   0xAARRGGBB
//     ││││││└ Blue  0–255
//     ││││└ Green 0–255
//     ││└ Red   0–255
//     └ Alpha 0=fully transparent … 255=fully opaque
//
// Alpha behaviour:
//   Alpha=255 (0xFF______) — fully opaque fast path, no readback needed.
//   Alpha=0   (0x00______) — GFX_TRANSPARENT: pixel is skipped entirely.
//   Alpha=1–254            — true alpha blend over existing framebuffer pixel.
//
// Backwards-compatible color helpers:
//   color565(r,g,b)            same call as before; now returns opaque ARGB8888
//                              with full 8-bit precision (no more 5/6-bit loss).
//   color888(r,g,b)            explicit RGB888, alpha=255.
//   colorRGB(r,g,b)            alias for color888.
//   colorARGB(a,r,g,b)         full ARGB control.
//
// Convenience color constants:
//   GFX_TRANSPARENT  0x00000000  skip pixel entirely
//   GFX_BLACK        0xFF000000
//   GFX_WHITE        0xFFFFFFFF
//   GFX_RED          0xFFFF0000
//   GFX_GREEN        0xFF00FF00
//   GFX_BLUE         0xFF0000FF
//   GFX_YELLOW       0xFFFFFF00
//   GFX_CYAN         0xFF00FFFF
//   GFX_MAGENTA      0xFFFF00FF
//
#define GFX_TRANSPARENT  0x00000000u
#define GFX_BLACK        0xFF000000u
#define GFX_WHITE        0xFFFFFFFFu
#define GFX_RED          0xFFFF0000u
#define GFX_GREEN        0xFF00FF00u
#define GFX_BLUE         0xFF0000FFu
#define GFX_YELLOW       0xFFFFFF00u
#define GFX_CYAN         0xFF00FFFFu
#define GFX_MAGENTA      0xFFFF00FFu

// ===== FONT STRUCTURES (Compatible with Adafruit GFX) ========================

typedef struct {
    uint16_t bitmapOffset;
    uint8_t  width, height;
    uint8_t  xAdvance;
    int8_t   xOffset, yOffset;
} GFXglyph;

typedef struct {
    uint8_t  *bitmap;
    GFXglyph *glyph;
    uint16_t  first, last;
    uint8_t   yAdvance;
} GFXfont;

// ===== EVENT SYSTEM (forward declaration - full definition after LinuxGFX) ===

enum class GFXEventType : uint8_t {
    MOUSE_MOVE = 0,
    MOUSE_BUTTON_DOWN = 1,
    MOUSE_BUTTON_UP = 2,
};

struct GFXInputEvent {
    GFXEventType type;
    int16_t x;      ///< Mouse X position or input resolution width
    int16_t y;      ///< Mouse Y position or input resolution height
    uint8_t button; ///< SDL button (0=left, 1=middle, 2=right)
};

/// Callback type for input events: void callback(const GFXInputEvent& event)
using GFXEventCallback = std::function<void(const GFXInputEvent&)>;

// ===== DRAW STYLE ===========================================================

/// Cap style applied to the open ends of stroked lines.
enum class GFXLineCap : uint8_t {
    Butt   = 0, ///< Flat end exactly at the endpoint (default)
    Round  = 1, ///< Semicircle of radius strokeWidth/2 at each endpoint
    Square = 2, ///< Flat end extended by strokeWidth/2 beyond the endpoint
};

/// Join style applied where two stroked segments meet.
enum class GFXLineJoin : uint8_t {
    Miter = 0, ///< Sharp mitered corner (default)
    Bevel = 1, ///< Bevelled (clipped) corner
    Round = 2, ///< Rounded corner
};

/**
 * Aggregate draw style applied by all stroke primitives.
 *
 * Set globally with setDrawStyle() / individual setters, or save/restore with
 * getDrawStyle() / setDrawStyle() for temporary overrides.
 *
 * Defaults (strokeWidth=1, lineCap=Butt, lineJoin=Miter, antiAlias=false)
 * reproduce the original single-pixel Bresenham behaviour so existing code
 * requires no changes.
 */
struct GFXDrawStyle {
    uint8_t     strokeWidth = 1;                  ///< Stroke width in pixels (1 = thin, default)
    GFXLineCap  lineCap     = GFXLineCap::Butt;   ///< Cap style for line/arc endpoints
    GFXLineJoin lineJoin    = GFXLineJoin::Miter;  ///< Join style at stroke corners
    bool        antiAlias   = false;              ///< Anti-aliased rendering (default off)
};

// ===== MULTI-BUFFER SUPPORT (Framebuffer back-end only) ======================

enum BufferIndex { BUFFER_0 = 0, BUFFER_1 = 1, BUFFER_2 = 2 };

typedef struct {
    uint32_t *pData;   ///< ARGB8888 pixel buffer
    bool      bOwned;
    bool      bReady;
} FrameBuffer;

// ===== LinuxGFX ===============================================================
/**
 * Adafruit-GFX-compatible graphics library for Linux (FB/SDL) / Windows (SDL).
 *
 * Color format: ARGB8888 (32-bit, 0xAARRGGBB).
 *   Full RGB888 output — no colour quantisation vs the old RGB565 mode.
 *   Per-pixel alpha blending on all primitives and text.
 *   Alpha=0 → pixel skipped (GFX_TRANSPARENT).
 *   Alpha=255 → fully opaque fast path (no readback).
 *   Alpha=1–254 → blended over existing framebuffer contents.
 *
 * Backwards compatibility:
 *   color565(r,g,b) still works — returns opaque ARGB8888 with full 8-bit RGB.
 *   Old uint16_t color variables → change to uint32_t (compiler will warn).
 *
 * Backwards-compatible alias: typedef LinuxGFX CircleGFX;
 */
class LinuxGFX {
public:
    /**
     * DRM/KMS back-end constructor (raw kernel ioctls, no Mesa/libdrm).
     * Opens the DRM card, enumerates connectors, creates two dumb buffers for
     * double-buffering, and page-flips on every swapBuffers() call.
     * Default device is /dev/dri/card0.
     * Build with -DGFX_USE_DRM (via CMake option GFX_DRM_SUPPORT=ON).
     */
#if defined(GFX_USE_DRM)
    explicit LinuxGFX(const char *drmdev = "/dev/dri/card0");
#elif defined(GFXSDL)
    explicit LinuxGFX(const char *title = "GFX Window", uint16_t width = 1024, uint16_t height = 768);
#else
    /**
     * Framebuffer back-end constructor (default).
     * @param fbdev  Framebuffer device node (e.g. "/dev/fb0").
     *               The framebuffer must be at 32 bpp.
     */
    explicit LinuxGFX(const char *fbdev = "/dev/fb0");
#endif

    virtual ~LinuxGFX();

    void stop();

    // ===== CORE DRAW API =====================================================

    virtual void drawPixel      (int16_t x, int16_t y, uint32_t color);
    void startWrite     (void);
    void endWrite       (void);
    void writePixel     (int16_t x, int16_t y, uint32_t color);
    void writeFillRect  (int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void writeFastVLine (int16_t x, int16_t y, int16_t h, uint32_t color);
    void writeFastHLine (int16_t x, int16_t y, int16_t w, uint32_t color);
    void writeLine      (int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);

    // ===== BASIC DRAW API ====================================================

    virtual void drawFastVLine  (int16_t x, int16_t y, int16_t h, uint32_t color);
    virtual void drawFastHLine  (int16_t x, int16_t y, int16_t w, uint32_t color);
    void drawLine       (int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);
    void drawRect       (int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void fillRect       (int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    virtual void fillScreen     (uint32_t color);

    void drawCircle     (int16_t x0, int16_t y0, int16_t r, uint32_t color);
    void fillCircle     (int16_t x0, int16_t y0, int16_t r, uint32_t color);

    /**
     * Draw an arc (partial circle outline).
     *
     * Angles are in degrees.  0° = +x axis (east / 3 o'clock).  Because the
     * screen y-axis points down, increasing angle sweeps CLOCKWISE
     * (90° = south / 6 o'clock).  The arc runs from startAngle clockwise to
     * endAngle — pass endAngle > startAngle for the natural direction.
     * A full turn (|endAngle - startAngle| == 360) draws a complete circle;
     * a zero span draws nothing.
     *
     * Honours the current draw style: strokeWidth > 1 renders a thick ring
     * band, antiAlias smooths the edges, and the default 1px/no-AA style
     * produces a crisp midpoint outline.
     */
    void drawArc        (int16_t x0, int16_t y0, int16_t r,
                         float startAngle, float endAngle, uint32_t color);

    void drawRoundRect  (int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius, uint32_t color);
    void fillRoundRect  (int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius, uint32_t color);

    void drawTriangle   (int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2, uint32_t color);
    void fillTriangle   (int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2, uint32_t color);

    // ===== BITMAP DRAW API ===================================================
    // 1-bit bitmaps: color/bg are ARGB8888 (bg=GFX_TRANSPARENT to skip bg pixels)

    void drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                    int16_t w, int16_t h, uint32_t color);
    void drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                    int16_t w, int16_t h, uint32_t color, uint32_t bg);
    void drawBitmap(int16_t x, int16_t y, uint8_t *bitmap,
                    int16_t w, int16_t h, uint32_t color);
    void drawBitmap(int16_t x, int16_t y, uint8_t *bitmap,
                    int16_t w, int16_t h, uint32_t color, uint32_t bg);

    void drawXBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                     int16_t w, int16_t h, uint32_t color);

    void drawGrayscaleBitmap(int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h);
    void drawGrayscaleBitmap(int16_t x, int16_t y, uint8_t *bitmap,        int16_t w, int16_t h);
    void drawGrayscaleBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                             const uint8_t mask[], int16_t w, int16_t h);
    void drawGrayscaleBitmap(int16_t x, int16_t y, uint8_t *bitmap,
                             uint8_t *mask, int16_t w, int16_t h);

    // RGB bitmaps: pixel data is ARGB8888 (uint32_t per pixel)
    void drawRGBBitmap(int16_t x, int16_t y, const uint32_t bitmap[], int16_t w, int16_t h);
    void drawRGBBitmap(int16_t x, int16_t y,       uint32_t *bitmap,  int16_t w, int16_t h);
    void drawRGBBitmap(int16_t x, int16_t y, const uint32_t bitmap[],
                       const uint8_t mask[], int16_t w, int16_t h);
    void drawRGBBitmap(int16_t x, int16_t y, uint32_t *bitmap,
                       uint8_t *mask, int16_t w, int16_t h);

    void drawRGB565Bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);

    // Textured triangle: bitmap is ARGB8888 (texW×texH).
    // p0→UV(0,0)  p1→UV(texW-1,0)  p2→UV(0,texH-1)
    void drawBitmapTriangle(int16_t x0, int16_t y0,
                            int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2,
                            const uint32_t *bitmap, int16_t texW, int16_t texH);
    void drawBitmapTriangle(int16_t x0, int16_t y0,
                            int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2,
                            uint32_t *bitmap, int16_t texW, int16_t texH);

    // ===== TEXT API ==========================================================

    void setCursor (int16_t x, int16_t y);
    void setTextColor (uint32_t c);                  ///< fg color; bg = GFX_TRANSPARENT
    void setTextColor (uint32_t c, uint32_t bg);     ///< pass GFX_TRANSPARENT for transparent bg
    void setTextColorTransparentBg(uint32_t c);      ///< convenience: fg only, transparent bg
    void setTextSize (uint8_t s);
    void setTextSize (uint8_t sx, uint8_t sy);
    void setTextWrap (bool w);
    void drawChar (int16_t x, int16_t y, unsigned char c,
                   uint32_t color, uint32_t bg, uint8_t size);
    void drawChar (int16_t x, int16_t y, unsigned char c,
                   uint32_t color, uint32_t bg, uint8_t size_x, uint8_t size_y);
    void writeText (const char *text);
    void setFont (const GFXfont *f = nullptr);
    void writeTextF(const char* fmt, ...);
    void setTextRotation(uint8_t rotation);  ///< Set text rotation in degrees (0, 90, 180, 270)
    uint8_t getTextRotation() const;         ///< Get current text rotation
    void setText(int16_t x, int16_t y, const char *text, 
                 uint32_t color = GFX_WHITE, uint32_t bgColor = GFX_TRANSPARENT, 
                 uint8_t sizeX = 1, uint8_t sizeY = 1, const GFXfont *font = nullptr,
                 uint8_t rotation = 0);

    // ===== PRINT API (convenience methods) ====================================
    
    void print   (const char *text);      ///< Write text, cursor stays on same line
    void println (const char *text);      ///< Write text, cursor advances to next line
    void println ();                      ///< Advance cursor to next line

    // ===== DRAW STYLE API ====================================================

    void               setDrawStyle  (const GFXDrawStyle &style); ///< Replace full style state
    const GFXDrawStyle &getDrawStyle () const;                     ///< Read current style
    void               setStrokeWidth(uint8_t width);  ///< Stroke width in pixels (1 = default)
    void               setLineCap    (GFXLineCap cap);  ///< Cap style for line endpoints
    void               setLineJoin   (GFXLineJoin join);///< Join style at corners
    void               setAntiAlias  (bool enable);    ///< Toggle anti-aliasing (default off)

    // ===== CONTROL API =======================================================

    void setRotation (uint8_t r);
    uint8_t getRotation (void) const;
    void invertDisplay(bool i);

    // ===== DIMENSION API =====================================================

    int16_t width() const;
    int16_t height() const;
    int16_t getCursorX() const;
    int16_t getCursorY() const;

    // ===== COLOR HELPERS =====================================================

    /// Backwards-compatible: same signature as before.
    /// Now returns full opaque ARGB8888 — no more 5/6-bit colour loss.
    static uint32_t color565(uint8_t r, uint8_t g, uint8_t b);
    static uint32_t color565(uint32_t rgb);          ///< color565(0xRRGGBB) → 0xFFRRGGBB

    static uint32_t color888(uint8_t r, uint8_t g, uint8_t b);             ///< alpha=255
    static uint32_t colorRGB(uint8_t r, uint8_t g, uint8_t b);            ///< alias for color888
    static uint32_t colorARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b); ///< full ARGB

    static uint32_t fromRGB565(uint16_t c);

    void swapBuffers(bool autoclear = true);

#ifdef GFXSDL
    // ===== SDL EVENT HANDLING ================================================

    /**
     * Process SDL events. Call this regularly in your main loop.
     * @return false if quit event received, true otherwise.
     */
    bool processEvents();

    /**
     * Register an event callback to receive input events.
     * @param callback Function to call on input events.
     */
    void setEventCallback(const GFXEventCallback& callback);

    /**
     * Get the current mouse position.
     * @param x Output: current X coordinate
     * @param y Output: current Y coordinate
     */
    void getMousePosition(int16_t& x, int16_t& y) const;
#endif

    // ===== MULTI-BUFFER API (Framebuffer back-end only) ======================

    bool      enableMultiBuffer    (uint8_t numBuffers = 2);
    bool      isMultiBuffered      () const;
    uint8_t   getBufferCount       () const;
    uint8_t   getDrawBufferIndex   () const;
    uint8_t   getDisplayBufferIndex() const;

    bool      selectDrawBuffer     (uint8_t bufferIndex);
    bool      selectDisplayBuffer  (uint8_t bufferIndex);

    void      clearBuffer          (int8_t bufferIndex = -1, uint32_t color = 0);
    uint32_t* getBuffer            (uint8_t bufferIndex);

    bool      attachExternalBuffer (uint8_t bufferIndex, uint32_t *pBuffer);
    bool      detachExternalBuffer (uint8_t bufferIndex);

protected:
    /// Protected tag-based constructor used by GFXcanvas to initialise the
    /// common LinuxGFX state WITHOUT opening any hardware device.
    struct CanvasTag {};
    explicit LinuxGFX(CanvasTag) noexcept;

    virtual void     setPixel (int16_t x, int16_t y, uint32_t color);
    virtual uint32_t getPixel (int16_t x, int16_t y) const;

    /// Helper function for writing rotated text
    void writeTextRotated(const char *text, int16_t originX, int16_t originY);

    /// Alpha-blend src over dst.  Both ARGB8888.  Returns blended ARGB8888.
    static inline uint32_t blendARGB(uint32_t src, uint32_t dst) {
        uint32_t sa = (src >> 24) & 0xFF;
        if (sa == 0xFF) return src;             // fully opaque fast path
        if (sa == 0x00) return dst;             // fully transparent fast path
        uint32_t da = 255u - sa;
        uint8_t r = (uint8_t)(((src >> 16 & 0xFF) * sa + (dst >> 16 & 0xFF) * da) / 255u);
        uint8_t g = (uint8_t)(((src >>  8 & 0xFF) * sa + (dst >>  8 & 0xFF) * da) / 255u);
        uint8_t b = (uint8_t)(((src       & 0xFF) * sa + (dst       & 0xFF) * da) / 255u);
        return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    void drawCircleHelper     (int16_t x0, int16_t y0, int16_t r,
                               uint8_t cornername, uint32_t color);
    void fillCircleHelper     (int16_t x0, int16_t y0, int16_t r,
                               uint8_t cornername, int16_t delta, uint32_t color);
    virtual void drawFastVLineInternal(int16_t x, int16_t y, int16_t h, uint32_t color);
    virtual void drawFastHLineInternal(int16_t x, int16_t y, int16_t w, uint32_t color);

    // ===== STYLED RENDERING HELPERS (called by draw* when style != default) ==

    /// Xiaolin Wu anti-aliased line (strokeWidth == 1, antiAlias == true).
    void writeLineAA     (int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);
    /// Thick solid line rendered as a filled parallelogram + optional caps.
    void writeThickLine  (int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          uint32_t color, uint8_t width);
    /// Anti-aliased 1-pixel-wide circle outline using a distance-field approach.
    void writeCircleAA   (int16_t x0, int16_t y0, int16_t r, uint32_t color);
    /// Thick circle outline (annular ring) filled with scan-line spans.
    void writeThickCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color, uint8_t width);
    /// Thick arc for one or more quadrants — used by drawRoundRect with strokeWidth > 1.
    /// cornermask: 0x1=top-left  0x2=top-right  0x4=bottom-right  0x8=bottom-left
    void writeThickArc   (int16_t x0, int16_t y0, int16_t r, uint8_t cornermask,
                          uint32_t color, uint8_t width);
    /// Anti-aliased 1-pixel arc outline (strokeWidth == 1, antiAlias == true).
    void writeArcAA      (int16_t x0, int16_t y0, int16_t r,
                          float startAngle, float sweep, uint32_t color);
    /// Thick arc outline (annular band) over an arbitrary angular span.
    void writeArcThick   (int16_t x0, int16_t y0, int16_t r,
                          float startAngle, float sweep, uint32_t color, uint8_t width);

    // ===== Framebuffer / multi-buffer members (always present) ================
    int       m_fbFd;
    uint8_t  *m_pFbMem;
    size_t    m_fbMemSize;
    uint32_t  m_depth;          ///< bits per pixel (must be 32)

    FrameBuffer m_buffers[3];
    uint8_t     m_bufferCount;
    uint8_t     m_drawBufferIndex;
    uint8_t     m_displayBufferIndex;
    bool        m_multiBufferEnabled;

    void _initializeMultiBuffer();
    void _cleanupMultiBuffer();
    void _flushToFb();

#ifdef GFX_USE_DRM
    void _drmWaitFlip();

    struct _DrmBuf {
        uint32_t handle;   ///< dumb buffer kernel handle
        uint32_t fb_id;    ///< DRM framebuffer object id
        uint8_t *map;      ///< mmap'd pointer into the dumb buffer
        uint32_t size;     ///< total byte size (height * pitch)
        uint32_t pitch;    ///< bytes per row (hardware-aligned)
    };

    int      m_drmFd;
    uint32_t m_drmCrtcId;
    uint32_t m_drmConnId;
    _DrmBuf  m_drmBufs[2];
    uint8_t  m_drmFront;        ///< index (0 or 1) of the buffer currently on-screen
    bool     m_drmFlipPending;
#endif

#ifdef GFXSDL
    // ===== SDL-only members ==================================================
    SDL_Window   *m_pWindow;
    SDL_Renderer *m_pRenderer;
    SDL_Texture  *m_pTexture;
    uint32_t     *m_pSurfaceBuffer;    ///< SDL surface pixel buffer (ARGB8888)

    GFXEventCallback m_eventCallback;
    int16_t          m_mouseX, m_mouseY;
    bool             m_shouldQuit;
#endif

    // ===== Common members ====================================================
    uint32_t  m_pitch;          ///< bytes per line
    uint32_t *m_pBuffer;        ///< current draw buffer (ARGB8888)
    int16_t  m_width, m_height;
    int16_t  m_cursorX, m_cursorY;
    uint32_t m_textColor, m_textBgColor;
    uint8_t  m_textSizeX, m_textSizeY;
    uint8_t  m_textRotation;    ///< text rotation in degrees (0, 90, 180, 270)
    bool     m_textWrap;
    uint8_t  m_rotation;
    bool     m_inverted;
    bool     m_inTransaction;

    const GFXfont *m_pFont;
    bool           m_fontSizeMultiplied;

    GFXDrawStyle   m_drawStyle; ///< Current draw style (strokeWidth, lineCap, lineJoin, AA)
};

// Backwards-compat alias
typedef LinuxGFX CircleGFX;

// =============================================================================
// GFXcanvas — Virtual off-screen framebuffer
// =============================================================================
//
// GFXcanvas provides an off-screen rendering surface that supports every draw
// primitive from LinuxGFX.  It inherits all drawing methods via the same shared
// implementation: setPixel / getPixel are overridden to target the heap buffer
// instead of a DRM/fb device.
//
// Five pixel formats are supported:
//
//   GFXcanvas::Format::BW        1-bit monochrome  — 1 byte per 8 pixels (MSB first)
//   GFXcanvas::Format::GRAY8     8-bit grayscale   — 1 byte per pixel
//   GFXcanvas::Format::RGB565    16-bit RGB565     — 2 bytes per pixel, little-endian
//   GFXcanvas::Format::RGB888    24-bit RGB         — 3 bytes per pixel, R,G,B order
//   GFXcanvas::Format::ARGB8888  32-bit ARGB        — 4 bytes per pixel (native format)
//
// All draw calls accept the same ARGB8888 uint32_t color values as LinuxGFX.
// Colors are converted to/from the canvas format transparently.
//
// Usage:
//   GFXcanvas canvas(320, 240, GFXcanvas::Format::RGB565);
//   canvas.fillScreen(GFX_BLACK);
//   canvas.drawRect(10, 10, 100, 50, GFX_WHITE);
//   // ... blit to LinuxGFX display:
//   gfx.drawRGBBitmap(0, 0, canvas.getBufferARGB8888(), canvas.width(), canvas.height());
//   // or for RGB565 canvas directly:
//   gfx.drawRGB565Bitmap(0, 0, canvas.getBufferRGB565(), canvas.width(), canvas.height());
//

class GFXcanvas : public LinuxGFX {
public:
    /// Pixel format of the backing buffer.
    enum class Format : uint8_t {
        BW       = 0,  ///< 1-bit monochrome (MSB-first, 1 byte per 8 pixels)
        GRAY8    = 1,  ///< 8-bit grayscale
        RGB565   = 2,  ///< 16-bit packed RGB (5R-6G-5B)
        RGB888   = 3,  ///< 24-bit RGB (3 bytes per pixel, R,G,B order)
        ARGB8888 = 4,  ///< 32-bit ARGB — native LinuxGFX format (default)
    };

    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------

    /**
     * @param w       Canvas width in pixels.
     * @param h       Canvas height in pixels.
     * @param fmt     Pixel format of the backing buffer (default: ARGB8888).
     * @param allocate_buffer  If false the buffer is not allocated; useful when
     *                         you want to attach an external buffer later via
     *                         attachBuffer().
     */
    GFXcanvas(uint16_t w, uint16_t h,
              Format   fmt              = Format::ARGB8888,
              bool     allocate_buffer  = true);

    ~GFXcanvas();

    // -------------------------------------------------------------------------
    // Core pixel operations  (override LinuxGFX hardware paths)
    // -------------------------------------------------------------------------

    void     drawPixel     (int16_t x, int16_t y, uint32_t color) override;
    void     fillScreen    (uint32_t color)                        override;
    void     drawFastVLine (int16_t x, int16_t y, int16_t h, uint32_t color) override;
    void     drawFastHLine (int16_t x, int16_t y, int16_t w, uint32_t color) override;

    /** Read a pixel back as ARGB8888 regardless of the canvas format. */
    uint32_t getPixel      (int16_t x, int16_t y) const override;

    // -------------------------------------------------------------------------
    // Buffer access
    // -------------------------------------------------------------------------

    /** Raw buffer pointer — byte layout depends on the canvas Format. */
    uint8_t *getBuffer()         const { return m_canvasBuffer; }

    /** Convenience: raw pointer typed as uint16_t (only valid for RGB565 format). */
    uint16_t *getBufferRGB565()  const;

    /** Convenience: raw pointer typed as uint32_t (only valid for ARGB8888 format). */
    uint32_t *getBufferARGB8888() const;

    /**
     * Copy the canvas contents into a caller-supplied ARGB8888 buffer.
     * @param dst   Output buffer; must be at least width()*height()*4 bytes.
     * @return true on success, false if the canvas has no buffer.
     */
    bool copyToARGB8888(uint32_t *dst) const;

    /**
     * Copy the canvas contents into a caller-supplied RGB565 buffer.
     * @param dst   Output buffer; must be at least width()*height()*2 bytes.
     * @return true on success.
     */
    bool copyToRGB565(uint16_t *dst) const;

    // -------------------------------------------------------------------------
    // External buffer attachment
    // -------------------------------------------------------------------------

    /**
     * Attach a pre-allocated external buffer.
     * The canvas does NOT take ownership — the caller must keep it alive.
     * Any previously owned internal buffer is freed.
     * @param buf  External buffer of at least bufferSize() bytes.
     * @return true on success.
     */
    bool attachBuffer(uint8_t *buf);

    /** Detach an external buffer and leave the canvas without a buffer. */
    void detachBuffer();

    // -------------------------------------------------------------------------
    // Metadata
    // -------------------------------------------------------------------------

    Format   format()     const { return m_format; }
    size_t   bufferSize() const { return m_bufferBytes; }

    /** Byte-swap every pixel value in-place (useful for RGB565 endian conversion). */
    void byteSwap();

protected:
    // Hooks used by all inherited LinuxGFX draw methods
    void     setPixelRaw   (int16_t x, int16_t y, uint32_t argb);
    uint32_t getPixelRaw   (int16_t x, int16_t y) const;

    // Override LinuxGFX protected pixel hooks so writePixel/writeFastHLine etc. work
    void     setPixel (int16_t x, int16_t y, uint32_t color) override;

    void drawFastVLineInternal(int16_t x, int16_t y, int16_t h, uint32_t color) override;
    void drawFastHLineInternal(int16_t x, int16_t y, int16_t w, uint32_t color) override;

private:
    Format   m_format;
    uint8_t *m_canvasBuffer;   ///< Heap buffer (owned when m_bufferOwned == true)
    size_t   m_bufferBytes;    ///< Total size in bytes
    bool     m_bufferOwned;    ///< True when we allocated the buffer ourselves

    // Colour conversion helpers
    static uint32_t argbFromGray8  (uint8_t  g);
    static uint32_t argbFromRGB565 (uint16_t c);
    static uint32_t argbFromRGB888 (uint8_t r, uint8_t g, uint8_t b);

    static uint8_t  gray8FromARGB  (uint32_t argb);
    static uint16_t rgb565FromARGB (uint32_t argb);
    static void     rgb888FromARGB (uint32_t argb, uint8_t &r, uint8_t &g, uint8_t &b);

    // Disable copy
    GFXcanvas(const GFXcanvas&)            = delete;
    GFXcanvas& operator=(const GFXcanvas&) = delete;
};

// Convenience type aliases (mirrors Adafruit GFX naming)
using GFXcanvasBW      = GFXcanvas;   ///< Use with Format::BW
using GFXcanvasGray    = GFXcanvas;   ///< Use with Format::GRAY8
using GFXcanvas16      = GFXcanvas;   ///< Use with Format::RGB565
using GFXcanvas24      = GFXcanvas;   ///< Use with Format::RGB888
using GFXcanvas32      = GFXcanvas;   ///< Use with Format::ARGB8888

#endif // GFX_H