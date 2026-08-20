#include "GFX.h"

// Belt-and-suspenders: define sentinels if an older header is in the path.
#ifndef GFX_TRANSPARENT
#  define GFX_TRANSPARENT 0x00000000u
#endif

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <cmath>
#include <algorithm>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <poll.h>
#endif

#ifdef GFXSDL
#include <SDL2/SDL.h>
#elif defined(GFX_USE_DRM)
// ---- DRM/KMS UAPI -- raw Linux kernel stable ABI, no libdrm required --------
// Structs and ioctl numbers from linux/include/uapi/drm/{drm.h,drm_mode.h}.
// These are frozen kernel ABI and will not change.
// _IOWR/_IOW come from <sys/ioctl.h> (included above).
#ifndef DRM_IOWR
#  define DRM_IOWR(nr,t) _IOWR('d', nr, t)
#  define DRM_IOW(nr,t)  _IOW('d',  nr, t)
#endif
#define DRM_DISPLAY_MODE_LEN 32
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[DRM_DISPLAY_MODE_LEN];
};
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection;   // 1 = connected
    uint32_t mm_width, mm_height, subpixel, pad;
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct drm_mode_map_dumb    { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb{ uint32_t handle; };
struct drm_mode_page_flip   {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};
struct drm_event            { uint32_t type, length; };
struct drm_event_vblank     {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};
#define DRM_IOCTL_MODE_GETRESOURCES  DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC       DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER    DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB         DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB          DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP     DRM_IOWR(0xB0, struct drm_mode_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB   DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB      DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB  DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_MODE_PAGE_FLIP_EVENT     0x01u
#define DRM_EVENT_FLIP_COMPLETE      0x02u
// ---- end DRM UAPI -----------------------------------------------------------
#else
#  ifndef _WIN32
#  include <linux/fb.h>
#  endif
#endif

#ifdef GFXSDL

LinuxGFX::LinuxGFX(const char *title, uint16_t width, uint16_t height)
    : m_fbFd(-1), m_pFbMem(nullptr), m_fbMemSize(0),
      m_bufferCount(1), m_drawBufferIndex(0), m_displayBufferIndex(0),
      m_multiBufferEnabled(false),
      m_pWindow(nullptr), m_pRenderer(nullptr), m_pTexture(nullptr),
      m_pSurfaceBuffer(nullptr), m_mouseX(0), m_mouseY(0), m_shouldQuit(false),
      m_pBuffer(nullptr),
      m_width(width), m_height(height),
      m_cursorX(0), m_cursorY(0),
      m_textColor(GFX_WHITE), m_textBgColor(GFX_TRANSPARENT),
      m_textSizeX(1), m_textSizeY(1), m_textRotation(0),
      m_textWrap(true), m_rotation(0),
      m_inverted(false), m_inTransaction(false),
      m_pFont(nullptr), m_fontSizeMultiplied(true)
{
    SDL_SetMainReady(); // companion to SDL_MAIN_HANDLED defined in GFX.h
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "GFX: SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    m_pWindow = SDL_CreateWindow(
        title ? title : "GFX Window",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN
    );

    if (!m_pWindow) {
        fprintf(stderr, "GFX: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }

    m_pRenderer = SDL_CreateRenderer(m_pWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_pRenderer) {
        fprintf(stderr, "GFX: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(m_pWindow);
        SDL_Quit();
        return;
    }

    m_pSurfaceBuffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));
    if (!m_pSurfaceBuffer) {
        fprintf(stderr, "GFX: calloc failed for surface buffer\n");
        SDL_DestroyRenderer(m_pRenderer);
        SDL_DestroyWindow(m_pWindow);
        SDL_Quit();
        return;
    }

    m_pTexture = SDL_CreateTexture(
        m_pRenderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );

    if (!m_pTexture) {
        fprintf(stderr, "GFX: SDL_CreateTexture failed: %s\n", SDL_GetError());
        free(m_pSurfaceBuffer);
        SDL_DestroyRenderer(m_pRenderer);
        SDL_DestroyWindow(m_pWindow);
        SDL_Quit();
        return;
    }

    m_pBuffer = m_pSurfaceBuffer;
    m_pitch = (uint32_t)width * sizeof(uint32_t);  
    _initializeMultiBuffer();  // Initialize buffer array to nullptrs (multi-buffer disabled for SDL)
    SDL_RenderClear(m_pRenderer);
	SDL_RenderCopy(m_pRenderer, m_pTexture, NULL, NULL);
	SDL_RenderPresent(m_pRenderer); // <--- RTSS intercepts this call!
	fprintf(stderr, "GFX: SDL window %s OK (%dx%d @ 32bpp ARGB8888)\n", title, width, height);
}

#elif !defined(GFX_USE_DRM)

LinuxGFX::LinuxGFX(const char *fbdev)
    : m_fbFd(-1), m_pFbMem(nullptr), m_fbMemSize(0),
      m_pitch(0), m_depth(0), m_pBuffer(nullptr),
      m_bufferCount(1), m_drawBufferIndex(0), m_displayBufferIndex(0),
      m_multiBufferEnabled(false),
      m_width(0), m_height(0),
      m_cursorX(0), m_cursorY(0),
      m_textColor(GFX_WHITE), m_textBgColor(GFX_TRANSPARENT),
      m_textSizeX(1), m_textSizeY(1), m_textRotation(0),
      m_textWrap(true), m_rotation(0),
      m_inverted(false), m_inTransaction(false),
      m_pFont(nullptr), m_fontSizeMultiplied(true)
{
    const char *dev = fbdev ? fbdev : "/dev/fb0";
    m_fbFd = open(dev, O_RDWR);
    if (m_fbFd < 0) {
        fprintf(stderr, "GFX: cannot open %s: %s\n", dev, strerror(errno));
        return;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(m_fbFd, FBIOGET_FSCREENINFO, &finfo) ||
        ioctl(m_fbFd, FBIOGET_VSCREENINFO, &vinfo)) {
        fprintf(stderr, "GFX: ioctl FBIOGET failed\n");
        close(m_fbFd); m_fbFd = -1; return;
    }

    if (vinfo.bits_per_pixel != 32) {
        vinfo.bits_per_pixel = 32;
        ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &vinfo);
        ioctl(m_fbFd, FBIOGET_FSCREENINFO, &finfo);
        ioctl(m_fbFd, FBIOGET_VSCREENINFO, &vinfo);
        if (vinfo.bits_per_pixel != 32) {
            fprintf(stderr, "GFX: framebuffer does not support 32 bpp\n");
            close(m_fbFd); m_fbFd = -1; return;
        }
    }

    m_width     = (int16_t)vinfo.xres;
    m_height    = (int16_t)vinfo.yres;
    m_pitch     = finfo.line_length;
    m_depth     = vinfo.bits_per_pixel;
    m_fbMemSize = (size_t)finfo.smem_len;

    m_pFbMem = (uint8_t*)mmap(nullptr, m_fbMemSize,
                               PROT_READ | PROT_WRITE, MAP_SHARED, m_fbFd, 0);
    if (m_pFbMem == MAP_FAILED) {
        fprintf(stderr, "GFX: mmap failed: %s\n", strerror(errno));
        m_pFbMem = nullptr; close(m_fbFd); m_fbFd = -1; return;
    }

    m_pBuffer = (uint32_t*)m_pFbMem;
    _initializeMultiBuffer();

    fprintf(stderr, "GFX: framebuffer %s OK (%dx%d @ %d bpp ARGB8888, pitch=%d)\n",
            dev, m_width, m_height, m_depth, m_pitch);
}
#endif // GFXSDL / GFX_USE_DRM / fbdev constructor selection

// =============================================================================
// DRM/KMS back-end constructor
// =============================================================================
#ifdef GFX_USE_DRM
LinuxGFX::LinuxGFX(const char *drmdev)
    : m_fbFd(-1), m_pFbMem(nullptr), m_fbMemSize(0),
      m_pitch(0), m_depth(32), m_pBuffer(nullptr),
      m_bufferCount(1), m_drawBufferIndex(0), m_displayBufferIndex(0),
      m_multiBufferEnabled(false),
      m_drmFd(-1), m_drmCrtcId(0), m_drmConnId(0),
      m_drmFront(0), m_drmFlipPending(false),
      m_width(0), m_height(0),
      m_cursorX(0), m_cursorY(0),
      m_textColor(GFX_WHITE), m_textBgColor(GFX_TRANSPARENT),
      m_textSizeX(1), m_textSizeY(1), m_textRotation(0),
      m_textWrap(true), m_rotation(0),
      m_inverted(false), m_inTransaction(false),
      m_pFont(nullptr), m_fontSizeMultiplied(true)
{
    memset(m_drmBufs, 0, sizeof(m_drmBufs));

    const char *dev = drmdev ? drmdev : "/dev/dri/card0";
    m_drmFd = open(dev, O_RDWR | O_CLOEXEC);
    if (m_drmFd < 0) {
        fprintf(stderr, "GFX/DRM: cannot open %s: %s\n", dev, strerror(errno));
        return;
    }

    // --- 1. Get resource counts (first call with zero ptrs) ------------------
    drm_mode_card_res res = {};
    if (ioctl(m_drmFd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        fprintf(stderr, "GFX/DRM: GETRESOURCES failed: %s\n", strerror(errno));
        close(m_drmFd); m_drmFd = -1; return;
    }
    if (res.count_connectors == 0 || res.count_crtcs == 0) {
        fprintf(stderr, "GFX/DRM: no connectors or CRTCs found\n");
        close(m_drmFd); m_drmFd = -1; return;
    }

    // --- 2. Fill in ID arrays (second call) ----------------------------------
    uint32_t conn_ids[16] = {}, crtc_ids[16] = {}, enc_ids[16] = {};
    uint32_t nc = res.count_connectors < 16 ? res.count_connectors : 16;
    uint32_t nk = res.count_crtcs      < 16 ? res.count_crtcs      : 16;
    uint32_t ne = res.count_encoders   < 16 ? res.count_encoders    : 16;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)enc_ids;
    res.count_connectors = nc;
    res.count_crtcs      = nk;
    res.count_encoders   = ne;
    if (ioctl(m_drmFd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        fprintf(stderr, "GFX/DRM: GETRESOURCES (2nd) failed: %s\n", strerror(errno));
        close(m_drmFd); m_drmFd = -1; return;
    }

    // --- 3. Find first connected connector with at least one mode ------------
    drm_mode_modeinfo sel_mode = {};
    for (uint32_t i = 0; i < nc && m_drmConnId == 0; i++) {
        drm_mode_get_connector c = {};
        c.connector_id = conn_ids[i];
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        if (c.connection != 1 || c.count_modes == 0) continue; // not connected

        // Second connector call: read up to 64 modes
        drm_mode_modeinfo modes[64] = {};
        uint32_t nm = c.count_modes < 64 ? c.count_modes : 64;
        drm_mode_get_connector c2 = {};
        c2.connector_id = conn_ids[i];
        c2.modes_ptr    = (uint64_t)(uintptr_t)modes;
        c2.count_modes  = nm;
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_GETCONNECTOR, &c2)) continue;

        m_drmConnId = conn_ids[i];
        sel_mode    = modes[0]; // first mode = preferred / highest res

        // Find CRTC via active encoder
        if (c.encoder_id) {
            drm_mode_get_encoder enc = {};
            enc.encoder_id = c.encoder_id;
            if (!ioctl(m_drmFd, DRM_IOCTL_MODE_GETENCODER, &enc)) {
                if (enc.crtc_id) {
                    m_drmCrtcId = enc.crtc_id;
                } else {
                    // encoder attached but no active CRTC — pick from possible_crtcs
                    for (uint32_t k = 0; k < nk && !m_drmCrtcId; k++)
                        if (enc.possible_crtcs & (1u << k))
                            m_drmCrtcId = crtc_ids[k];
                }
            }
        }
        // Fallback: use first available CRTC
        if (!m_drmCrtcId && nk > 0)
            m_drmCrtcId = crtc_ids[0];
    }

    if (!m_drmConnId || !m_drmCrtcId) {
        fprintf(stderr, "GFX/DRM: no active connector/CRTC found\n");
        close(m_drmFd); m_drmFd = -1; return;
    }

    m_width  = (int16_t)sel_mode.hdisplay;
    m_height = (int16_t)sel_mode.vdisplay;

    // --- 4. Create two dumb buffers (double-buffering) -----------------------
    for (int i = 0; i < 2; i++) {
        drm_mode_create_dumb dumb = {};
        dumb.width  = (uint32_t)m_width;
        dumb.height = (uint32_t)m_height;
        dumb.bpp    = 32;
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
            fprintf(stderr, "GFX/DRM: CREATE_DUMB[%d] failed: %s\n", i, strerror(errno));
            goto drm_init_fail;
        }
        m_drmBufs[i].handle = dumb.handle;
        m_drmBufs[i].pitch  = dumb.pitch;
        m_drmBufs[i].size   = (uint32_t)dumb.size;

        drm_mode_fb_cmd fb = {};
        fb.width  = (uint32_t)m_width;
        fb.height = (uint32_t)m_height;
        fb.pitch  = dumb.pitch;
        fb.bpp    = 32;
        fb.depth  = 24; // XRGB8888 — alpha byte ignored by display
        fb.handle = dumb.handle;
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_ADDFB, &fb)) {
            fprintf(stderr, "GFX/DRM: ADDFB[%d] failed: %s\n", i, strerror(errno));
            goto drm_init_fail;
        }
        m_drmBufs[i].fb_id = fb.fb_id;

        drm_mode_map_dumb mapd = {};
        mapd.handle = dumb.handle;
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapd)) {
            fprintf(stderr, "GFX/DRM: MAP_DUMB[%d] failed: %s\n", i, strerror(errno));
            goto drm_init_fail;
        }

        void *ptr = mmap(nullptr, dumb.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, m_drmFd, (off_t)mapd.offset);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "GFX/DRM: mmap[%d] failed: %s\n", i, strerror(errno));
            m_drmBufs[i].map = nullptr;
            goto drm_init_fail;
        }
        m_drmBufs[i].map = (uint8_t*)ptr;
        memset(m_drmBufs[i].map, 0, dumb.size);
    }

    // --- 5. Initial mode-set: display buffer 0 -------------------------------
    {
        uint32_t conn_arr[1] = { m_drmConnId };
        drm_mode_crtc crtc = {};
        crtc.crtc_id             = m_drmCrtcId;
        crtc.fb_id               = m_drmBufs[0].fb_id;
        crtc.set_connectors_ptr  = (uint64_t)(uintptr_t)conn_arr;
        crtc.count_connectors    = 1;
        crtc.mode                = sel_mode;
        crtc.mode_valid          = 1;
        if (ioctl(m_drmFd, DRM_IOCTL_MODE_SETCRTC, &crtc)) {
            fprintf(stderr, "GFX/DRM: SETCRTC failed: %s\n", strerror(errno));
            goto drm_init_fail;
        }
    }

    m_drmFront = 0;                               // buf 0 is on screen
    m_pitch    = m_drmBufs[0].pitch;
    m_pBuffer  = (uint32_t*)m_drmBufs[1].map;    // draw into buf 1
    _initializeMultiBuffer();

    fprintf(stderr, "GFX/DRM: %s OK  conn=%u crtc=%u  %dx%d  pitch=%u  bufs=[%u,%u]\n",
            dev, m_drmConnId, m_drmCrtcId,
            m_width, m_height, m_pitch,
            m_drmBufs[0].fb_id, m_drmBufs[1].fb_id);
    return;

drm_init_fail:
    // Release anything allocated so far; m_drmFd stays open until stop()
    for (int i = 0; i < 2; i++) {
        if (m_drmBufs[i].map)   munmap(m_drmBufs[i].map, m_drmBufs[i].size);
        if (m_drmBufs[i].fb_id) ioctl(m_drmFd, DRM_IOCTL_MODE_RMFB, &m_drmBufs[i].fb_id);
        if (m_drmBufs[i].handle) {
            drm_mode_destroy_dumb dd = { m_drmBufs[i].handle };
            ioctl(m_drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }
    memset(m_drmBufs, 0, sizeof(m_drmBufs));
    close(m_drmFd); m_drmFd = -1;
}
#endif // GFX_USE_DRM

void LinuxGFX::stop() {
    _cleanupMultiBuffer();

#if defined(GFX_USE_DRM)
    if (m_drmFd < 0) return;
    if (m_drmFlipPending) _drmWaitFlip();
    for (int i = 0; i < 2; i++) {
        if (m_drmBufs[i].map && m_drmBufs[i].size)
            munmap(m_drmBufs[i].map, m_drmBufs[i].size);
        if (m_drmBufs[i].fb_id)
            ioctl(m_drmFd, DRM_IOCTL_MODE_RMFB, &m_drmBufs[i].fb_id);
        if (m_drmBufs[i].handle) {
            drm_mode_destroy_dumb dd = { m_drmBufs[i].handle };
            ioctl(m_drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }
    close(m_drmFd); m_drmFd = -1;
#elif defined(GFXSDL)
    // Only the object that created a window owns the SDL backend. A GFXcanvas has
    // m_pWindow == nullptr and must not destroy resources or call SDL_Quit()
    // (which would tear SDL down while a real window is still alive).
    if (m_pWindow) {
        if (m_pTexture) SDL_DestroyTexture(m_pTexture);
        if (m_pRenderer) SDL_DestroyRenderer(m_pRenderer);
        SDL_DestroyWindow(m_pWindow);
        if (m_pSurfaceBuffer) free(m_pSurfaceBuffer);
        m_pTexture = nullptr;
        m_pRenderer = nullptr;
        m_pWindow = nullptr;
        m_pSurfaceBuffer = nullptr;
        SDL_Quit();
    }
#else
    if (m_pFbMem && m_pFbMem != MAP_FAILED) munmap(m_pFbMem, m_fbMemSize);
    if (m_fbFd >= 0) close(m_fbFd);
#endif
}

LinuxGFX::~LinuxGFX() {
    stop();
}

void LinuxGFX::setPixel(int16_t x, int16_t y, uint32_t color) {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height || !m_pBuffer) return;
    m_pBuffer[(uint32_t)y * (m_pitch / 4) + (uint32_t)x] = color;
}

uint32_t LinuxGFX::getPixel(int16_t x, int16_t y) const {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height || !m_pBuffer) return 0;
    return m_pBuffer[(uint32_t)y * (m_pitch / 4) + (uint32_t)x];
}

void LinuxGFX::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    if ((color >> 24) == 0) return;
    for (int16_t i = y; i < y + h; i++) writeFastHLine(x, i, w, color);
}

void LinuxGFX::fillScreen(uint32_t color) {
    if ((color >> 24) == 0) return;
    fillRect(0, 0, m_width, m_height, color);
}

void LinuxGFX::drawRGBBitmap(int16_t x, int16_t y, const uint32_t bitmap[],
                              int16_t w, int16_t h) {
    startWrite();
    for (int16_t j = 0; j < h; j++, y++)
        for (int16_t i = 0; i < w; i++)
            writePixel(x + i, y, bitmap[j * w + i]);
    endWrite();
}

void LinuxGFX::drawRGBBitmap(int16_t x, int16_t y, uint32_t *bitmap, int16_t w, int16_t h) {
    drawRGBBitmap(x, y, (const uint32_t*)bitmap, w, h);
}

void LinuxGFX::_flushToFb() {
#if defined(GFX_USE_DRM)
    if (m_drmFd < 0 || !m_pBuffer) return;

    // Wait for any prior flip before issuing a new one
    if (m_drmFlipPending) _drmWaitFlip();

    uint8_t back = 1u - m_drmFront; // the buffer we just drew into

    drm_mode_page_flip flip = {};
    flip.crtc_id = m_drmCrtcId;
    flip.fb_id   = m_drmBufs[back].fb_id;
    flip.flags   = DRM_MODE_PAGE_FLIP_EVENT;

    if (ioctl(m_drmFd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
        m_drmFlipPending = true;
        _drmWaitFlip(); // block until vsync — prevents tearing
    } else {
        fprintf(stderr, "GFX/DRM: PAGE_FLIP failed: %s\n", strerror(errno));
    }

    m_drmFront = back;
    m_pBuffer  = (uint32_t*)m_drmBufs[1u - m_drmFront].map;
#elif defined(GFXSDL)
    if (!m_pTexture || !m_pSurfaceBuffer || !m_pRenderer) return;
    SDL_SetRenderDrawColor(m_pRenderer, 0, 0, 0, 255);
    SDL_RenderClear(m_pRenderer);
    SDL_UpdateTexture(m_pTexture, nullptr, m_pSurfaceBuffer, m_width * sizeof(uint32_t));
    SDL_RenderCopy(m_pRenderer, m_pTexture, nullptr, nullptr);
    SDL_RenderPresent(m_pRenderer);
#else
    if (!m_pFbMem || !m_pBuffer) return;
    if ((uint8_t*)m_pBuffer == m_pFbMem) return;
    if (m_multiBufferEnabled) {
        memcpy(m_pFbMem, m_buffers[m_displayBufferIndex].pData,
               (size_t)m_width * (size_t)m_height * 4);
    } else {
        memcpy(m_pFbMem, m_pBuffer, (size_t)m_width * (size_t)m_height * 4);
    }
#endif
}

#ifdef GFX_USE_DRM
void LinuxGFX::_drmWaitFlip() {
    struct pollfd pfd = { m_drmFd, POLLIN, 0 };
    if (poll(&pfd, 1, 1000) <= 0) { m_drmFlipPending = false; return; }

    uint8_t evbuf[256];
    ssize_t n = read(m_drmFd, evbuf, sizeof(evbuf));
    int off = 0;
    while (off + (int)sizeof(drm_event) <= (int)n) {
        const drm_event *ev = (const drm_event*)(evbuf + off);
        if (ev->length < (uint32_t)sizeof(drm_event) ||
            off + (int)ev->length > (int)n) break;
        if (ev->type == DRM_EVENT_FLIP_COMPLETE) m_drmFlipPending = false;
        off += (int)ev->length;
    }
}
#endif

void LinuxGFX::_initializeMultiBuffer() {
    for (uint8_t i = 0; i < 3; i++) {
        m_buffers[i].pData = nullptr;
        m_buffers[i].bOwned = false;
        m_buffers[i].bReady = false;
    }
    m_bufferCount = 1; m_drawBufferIndex = 0; m_displayBufferIndex = 0;
    m_multiBufferEnabled = false;
    m_buffers[0].pData = m_pBuffer; m_buffers[0].bOwned = false;
}

void LinuxGFX::_cleanupMultiBuffer() {
    for (uint8_t i = 0; i < 3; i++) {
        if (m_buffers[i].bOwned && m_buffers[i].pData) {
            free(m_buffers[i].pData);
            m_buffers[i].pData = nullptr;
        }
    }
    m_multiBufferEnabled = false;
}

bool LinuxGFX::enableMultiBuffer(uint8_t numBuffers) {
#if defined(GFX_USE_DRM)
    // DRM already double-buffers internally via page-flip; no extra allocation needed.
    (void)numBuffers;
    return true;
#elif defined(GFXSDL)
    // Allocate a real back buffer so drawing doesn't go directly to m_pSurfaceBuffer
    size_t bufSize = (size_t)m_width * (size_t)m_height * sizeof(uint32_t);
    
    // Free any previous back buffer
    for (uint8_t i = 0; i < 3; i++) {
        if (m_buffers[i].bOwned && m_buffers[i].pData) {
            free(m_buffers[i].pData);
            m_buffers[i].pData = nullptr;
            m_buffers[i].bOwned = false;
        }
    }

    // Buffer 0 = front (m_pSurfaceBuffer, not owned)
    m_buffers[0].pData  = m_pSurfaceBuffer;
    m_buffers[0].bOwned = false;
    m_buffers[0].bReady = false;

    // Buffer 1 = back (heap allocated)
    m_buffers[1].pData  = (uint32_t*)calloc(m_width * m_height, sizeof(uint32_t));
    m_buffers[1].bOwned = true;
    m_buffers[1].bReady = false;

    if (!m_buffers[1].pData) return false;

    m_bufferCount        = 2;
    m_displayBufferIndex = 0;
    m_drawBufferIndex    = 1;
    m_multiBufferEnabled = true;
    m_pBuffer            = m_buffers[1].pData;  // draw to back buffer
    return true;
#else
    // Framebuffer mode - use true multi-buffer
    if (numBuffers < 1 || numBuffers > 3) numBuffers = 2;
    size_t bufSize = (size_t)m_width * (size_t)m_height * sizeof(uint32_t);

    for (uint8_t i = 0; i < m_bufferCount; i++)
        if (m_buffers[i].bOwned && m_buffers[i].pData) {
            free(m_buffers[i].pData);
            m_buffers[i].pData = nullptr; m_buffers[i].bOwned = false;
        }

    m_bufferCount = numBuffers;
    for (uint8_t i = 0; i < m_bufferCount; i++) {
        m_buffers[i].pData = (uint32_t*)malloc(bufSize);
        if (!m_buffers[i].pData) {
            for (uint8_t j = 0; j < i; j++) { free(m_buffers[j].pData); m_buffers[j].pData = nullptr; }
            m_bufferCount = 1; m_buffers[0].pData = m_pBuffer; m_buffers[0].bOwned = false;
            m_multiBufferEnabled = false; return false;
        }
        m_buffers[i].bOwned = true; m_buffers[i].bReady = false;
        memset(m_buffers[i].pData, 0, bufSize);
    }
    // Start with draw and display buffers at different indices to avoid concurrent access
    m_displayBufferIndex = 0;  // Display shows buffer 0 (cleared)
    m_drawBufferIndex = (numBuffers > 1) ? 1 : 0;  // Draw to buffer 1 (or 0 if only 1 buffer)
    m_multiBufferEnabled = true;
    m_pBuffer = m_buffers[m_drawBufferIndex].pData;
    return true;
#endif
}

bool     LinuxGFX::isMultiBuffered()        const { return m_multiBufferEnabled; }
uint8_t  LinuxGFX::getBufferCount()         const { return m_bufferCount; }
uint8_t  LinuxGFX::getDrawBufferIndex()     const { return m_drawBufferIndex; }
uint8_t  LinuxGFX::getDisplayBufferIndex()  const { return m_displayBufferIndex; }

void LinuxGFX::swapBuffers(bool autoclear) {
    if (!m_multiBufferEnabled) { _flushToFb(); return; }

    if (m_drawBufferIndex >= m_bufferCount || !m_buffers[m_drawBufferIndex].pData) return;

    m_buffers[m_drawBufferIndex].bReady = true;
    m_displayBufferIndex = m_drawBufferIndex;

#ifdef GFXSDL
    // Copy back buffer -> front (m_pSurfaceBuffer) then present
    if (m_pSurfaceBuffer && m_buffers[m_displayBufferIndex].pData) {
        memcpy(m_pSurfaceBuffer, m_buffers[m_displayBufferIndex].pData,
               (size_t)m_width * (size_t)m_height * 4);
    }
    _flushToFb();  // does SDL_UpdateTexture + RenderPresent
#else
    if (m_pFbMem && m_buffers[m_displayBufferIndex].pData)
        memcpy(m_pFbMem, m_buffers[m_displayBufferIndex].pData,
               (size_t)m_width * (size_t)m_height * 4);
#endif

    m_drawBufferIndex = (m_drawBufferIndex + 1) % m_bufferCount;
    if (autoclear && m_buffers[m_drawBufferIndex].pData)
        memset(m_buffers[m_drawBufferIndex].pData, 0,
               (size_t)m_width * (size_t)m_height * 4);
    if (m_buffers[m_drawBufferIndex].pData)
        m_pBuffer = m_buffers[m_drawBufferIndex].pData;
}

bool LinuxGFX::selectDrawBuffer(uint8_t idx) {
    if (!m_multiBufferEnabled || idx >= m_bufferCount) return false;
    m_drawBufferIndex = idx; m_pBuffer = m_buffers[idx].pData; return true;
}

bool LinuxGFX::selectDisplayBuffer(uint8_t idx) {
    if (!m_multiBufferEnabled || idx >= m_bufferCount) return false;
    m_displayBufferIndex = idx;
    if (m_pFbMem)
        memcpy(m_pFbMem, m_buffers[idx].pData, (size_t)m_width * (size_t)m_height * 4);
    return true;
}

void LinuxGFX::clearBuffer(int8_t idx, uint32_t color) {
    uint32_t n = (uint32_t)m_width * (uint32_t)m_height;
    auto clearOne = [&](uint8_t i) {
        if (!m_buffers[i].pData) return;
        if (color == GFX_BLACK) memset(m_buffers[i].pData, 0, n * 4);
        else for (uint32_t j = 0; j < n; j++) m_buffers[i].pData[j] = color;
    };
    if (idx == -1)       { for (uint8_t i = 0; i < m_bufferCount; i++) clearOne(i); }
    else if (idx == -2)  { clearOne(m_drawBufferIndex); }
    else if ((uint8_t)idx < m_bufferCount) clearOne((uint8_t)idx);
}

uint32_t* LinuxGFX::getBuffer(uint8_t idx) {
    return idx < m_bufferCount ? m_buffers[idx].pData : nullptr;
}

bool LinuxGFX::attachExternalBuffer(uint8_t idx, uint32_t *pBuf) {
    if (idx >= 3 || !pBuf) return false;
    if (m_buffers[idx].bOwned && m_buffers[idx].pData) free(m_buffers[idx].pData);
    m_buffers[idx].pData = pBuf; m_buffers[idx].bOwned = false; m_buffers[idx].bReady = false;
    if (idx >= m_bufferCount) m_bufferCount = idx + 1;
    return true;
}

bool LinuxGFX::detachExternalBuffer(uint8_t idx) {
    if (idx >= m_bufferCount) return false;
    if (!m_buffers[idx].bOwned) {
        m_buffers[idx].pData = nullptr; m_buffers[idx].bReady = false; return true;
    }
    return false;
}

//  COMMON CODE  (identical for both back-ends)

void LinuxGFX::startWrite(void) { m_inTransaction = true;  }
void LinuxGFX::endWrite  (void) { m_inTransaction = false; }

void LinuxGFX::drawPixel(int16_t x, int16_t y, uint32_t color) {
    startWrite(); writePixel(x, y, color); endWrite();
}

// writePixel — the universal pixel write entry point.
// Handles transparency check and alpha blending.
void LinuxGFX::writePixel(int16_t x, int16_t y, uint32_t color) {
    uint32_t alpha = color >> 24;
    if (alpha == 0) return;   // fully transparent — skip
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
    if (alpha == 0xFF) {
        setPixel(x, y, color);
    } else {
        setPixel(x, y, blendARGB(color, getPixel(x, y)));
    }
}

void LinuxGFX::writeFastVLine(int16_t x, int16_t y, int16_t h, uint32_t color) {
    if ((color >> 24) == 0) return;
    for (int16_t i = y; i < y + h; i++) writePixel(x, i, color);
}

void LinuxGFX::writeFastHLine(int16_t x, int16_t y, int16_t w, uint32_t color) {
    if ((color >> 24) == 0) return;
    if (y < 0 || y >= m_height) return;
    int16_t xs = std::max<int16_t>(0, x);
    int16_t xe = std::min<int16_t>(m_width, x + w);
    for (int16_t i = xs; i < xe; i++) writePixel(i, y, color);
}

void LinuxGFX::writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color) {
    int16_t dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int16_t err = dx - dy, x = x0, y = y0;
    while (true) {
        writePixel(x, y, color);
        if (x == x1 && y == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

void LinuxGFX::drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t c) {
    startWrite();
    uint8_t sw = m_drawStyle.strokeWidth;
    if (sw > 1) {
        int16_t half = (int16_t)(sw / 2);
        writeFillRect((int16_t)(x - half), y, (int16_t)sw, h, c);
    } else {
        writeFastVLine(x, y, h, c);
    }
    endWrite();
}

void LinuxGFX::drawFastHLine(int16_t x, int16_t y, int16_t w, uint32_t c) {
    startWrite();
    uint8_t sw = m_drawStyle.strokeWidth;
    if (sw > 1) {
        int16_t half = (int16_t)(sw / 2);
        writeFillRect(x, (int16_t)(y - half), w, (int16_t)sw, c);
    } else {
        writeFastHLine(x, y, w, c);
    }
    endWrite();
}

void LinuxGFX::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t c) {
    startWrite();
    uint8_t sw = m_drawStyle.strokeWidth;
    if (sw > 1) {
        writeThickLine(x0, y0, x1, y1, c, sw);
    } else if (m_drawStyle.antiAlias) {
        writeLineAA(x0, y0, x1, y1, c);
    } else {
        writeLine(x0, y0, x1, y1, c);
    }
    endWrite();
}

void LinuxGFX::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c) {
    drawFastHLine(x, y,       w, c); drawFastHLine(x, y + h - 1, w, c);
    drawFastVLine(x, y,       h, c); drawFastVLine(x + w - 1, y, h, c);
}

void LinuxGFX::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c) {
    startWrite(); writeFillRect(x, y, w, h, c); endWrite();
}

// Circles
void LinuxGFX::drawCircleHelper(int16_t x0, int16_t y0, int16_t r,
                                 uint8_t c, uint32_t color) {
    int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddy += 2; f += ddy; }
        x++; ddx += 2; f += ddx;
        if (c & 0x4) { writePixel(x0+x,y0+y,color); writePixel(x0+y,y0+x,color); }
        if (c & 0x2) { writePixel(x0+x,y0-y,color); writePixel(x0+y,y0-x,color); }
        if (c & 0x8) { writePixel(x0-y,y0+x,color); writePixel(x0-x,y0+y,color); }
        if (c & 0x1) { writePixel(x0-y,y0-x,color); writePixel(x0-x,y0-y,color); }
    }
}

void LinuxGFX::fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                                  uint8_t c, int16_t delta, uint32_t color) {
    int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddy += 2; f += ddy; }
        x++; ddx += 2; f += ddx;
        if (c & 0x1) {
            writeFastVLine(x0+x, y0-y, 2*y+1+delta, color);
            writeFastVLine(x0+y, y0-x, 2*x+1+delta, color);
        }
        if (c & 0x2) {
            writeFastVLine(x0-x, y0-y, 2*y+1+delta, color);
            writeFastVLine(x0-y, y0-x, 2*x+1+delta, color);
        }
    }
}

void LinuxGFX::drawCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color) {
    startWrite();
    uint8_t sw = m_drawStyle.strokeWidth;
    if (sw > 1) {
        writeThickCircle(x0, y0, r, color, sw);
    } else if (m_drawStyle.antiAlias) {
        writeCircleAA(x0, y0, r, color);
    } else {
        int16_t f = 1-r, ddx = 1, ddy = -2*r, x = 0, y = r;
        writePixel(x0,y0+r,color); writePixel(x0,y0-r,color);
        writePixel(x0+r,y0,color); writePixel(x0-r,y0,color);
        while (x < y) {
            if (f >= 0) { y--; ddy += 2; f += ddy; }
            x++; ddx += 2; f += ddx;
            writePixel(x0+x,y0+y,color); writePixel(x0-x,y0+y,color);
            writePixel(x0+x,y0-y,color); writePixel(x0-x,y0-y,color);
            writePixel(x0+y,y0+x,color); writePixel(x0-y,y0+x,color);
            writePixel(x0+y,y0-x,color); writePixel(x0-y,y0-x,color);
        }
    }
    endWrite();
}

void LinuxGFX::fillCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color) {
    startWrite();
    writeFastVLine(x0, y0-r, 2*r+1, color);
    fillCircleHelper(x0, y0, r, 3, 0, color);
    endWrite();
}

// Arcs
// Angle convention: degrees, 0 = +x (east); increasing angle sweeps clockwise
// (screen y points down). See drawArc() doc comment in GFX.h.
void LinuxGFX::drawArc(int16_t x0, int16_t y0, int16_t r,
                        float startAngle, float endAngle, uint32_t color) {
    if (r < 0) return;
    float rawSweep = endAngle - startAngle;
    if (fabsf(rawSweep) < 1e-4f) return;          // degenerate span → nothing
    float sweep = fmodf(rawSweep, 360.0f);
    if (sweep <= 0.0f) sweep += 360.0f;           // normalize to (0, 360]

    startWrite();
    uint8_t sw = m_drawStyle.strokeWidth;
    if (sw > 1) {
        writeArcThick(x0, y0, r, startAngle, sweep, color, sw);
    } else if (m_drawStyle.antiAlias) {
        writeArcAA(x0, y0, r, startAngle, sweep, color);
    } else if (r == 0) {
        writePixel(x0, y0, color);
    } else {
        // Crisp single-pixel outline: midpoint circle points, angle-gated.
        const float R2D  = 57.29577951308232f;
        const float eps  = 0.5f * R2D / (float)r;  // ~half-pixel, in degrees
        const bool  full = (sweep >= 359.9999f);
        const float endN = startAngle + sweep;
        auto wrap180 = [](float a) -> float {
            float t = fmodf(a + 180.0f, 360.0f);
            if (t < 0.0f) t += 360.0f;
            return t - 180.0f;
        };
        auto plot = [&](int16_t px, int16_t py) {
            if (!full) {
                float pa = atan2f((float)(py - y0), (float)(px - x0)) * R2D;
                if (wrap180(pa - startAngle) < -eps) return;
                if (wrap180(endN - pa)       < -eps) return;
            }
            writePixel(px, py, color);
        };
        int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
        plot(x0, y0 + r); plot(x0, y0 - r);
        plot(x0 + r, y0); plot(x0 - r, y0);
        while (x < y) {
            if (f >= 0) { y--; ddy += 2; f += ddy; }
            x++; ddx += 2; f += ddx;
            plot(x0 + x, y0 + y); plot(x0 - x, y0 + y);
            plot(x0 + x, y0 - y); plot(x0 - x, y0 - y);
            plot(x0 + y, y0 + x); plot(x0 - y, y0 + x);
            plot(x0 + y, y0 - x); plot(x0 - y, y0 - x);
        }
    }
    endWrite();
}

// Rounded rectangles
void LinuxGFX::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t r, uint32_t color) {
    startWrite();
    int16_t mx = ((w<h)?w:h)/2; if (r > mx) r = mx;
    uint8_t sw = m_drawStyle.strokeWidth;

    if (sw <= 1) {
        writeFastHLine(x+r,   y,       w-2*r, color);
        writeFastHLine(x+r,   y+h-1,   w-2*r, color);
        writeFastVLine(x,     y+r,     h-2*r, color);
        writeFastVLine(x+w-1, y+r,     h-2*r, color);
        drawCircleHelper(x+r,     y+r,     r, 1, color);
        drawCircleHelper(x+w-r-1, y+r,     r, 2, color);
        drawCircleHelper(x+w-r-1, y+h-r-1, r, 4, color);
        drawCircleHelper(x+r,     y+h-r-1, r, 8, color);
    } else {
        int16_t half = (int16_t)(sw / 2);
        // Straight sides — thick filled rectangles
        writeFillRect((int16_t)(x+r),     (int16_t)(y     - half), (int16_t)(w-2*r), (int16_t)sw, color);
        writeFillRect((int16_t)(x+r),     (int16_t)(y+h-1 - half), (int16_t)(w-2*r), (int16_t)sw, color);
        writeFillRect((int16_t)(x     - half), (int16_t)(y+r), (int16_t)sw, (int16_t)(h-2*r), color);
        writeFillRect((int16_t)(x+w-1 - half), (int16_t)(y+r), (int16_t)sw, (int16_t)(h-2*r), color);
        // Corner arcs — thick quadrant rings
        writeThickArc(x+r,     y+r,     r, 0x1, color, sw);
        writeThickArc(x+w-r-1, y+r,     r, 0x2, color, sw);
        writeThickArc(x+w-r-1, y+h-r-1, r, 0x4, color, sw);
        writeThickArc(x+r,     y+h-r-1, r, 0x8, color, sw);
    }
    endWrite();
}

void LinuxGFX::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t r, uint32_t color) {
    startWrite();
    int16_t mx = ((w<h)?w:h)/2; if(r>mx) r=mx;
    writeFillRect(x+r, y, w-2*r, h, color);
    fillCircleHelper(x+w-r-1, y+r, r, 1, h-2*r-1, color);
    fillCircleHelper(x+r,     y+r, r, 2, h-2*r-1, color);
    endWrite();
}

// Triangles
void LinuxGFX::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2, uint32_t color) {
    drawLine(x0,y0,x1,y1,color); drawLine(x1,y1,x2,y2,color); drawLine(x2,y2,x0,y0,color);
}

void LinuxGFX::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2, uint32_t color) {
    if (y0>y1){std::swap(y0,y1);std::swap(x0,x1);}
    if (y1>y2){std::swap(y1,y2);std::swap(x1,x2);}
    if (y0>y1){std::swap(y0,y1);std::swap(x0,x1);}
    if (y0==y2) {
        int16_t a=std::min(x0,std::min(x1,x2)), b=std::max(x0,std::max(x1,x2));
        drawFastHLine(a,y0,b-a+1,color); return;
    }
    startWrite();
    int16_t dx01=x1-x0,dy01=y1-y0,dx02=x2-x0,dy02=y2-y0,dx12=x2-x1,dy12=y2-y1;
    int32_t sa=0,sb=0;
    int16_t last=(y1==y2)?y1:y1-1;
    for (int16_t y=y0;y<=last;y++) {
        int16_t a=x0+sa/dy01,b=x0+sb/dy02; sa+=dx01; sb+=dx02;
        if (a>b) std::swap(a,b);
        writeFastHLine(a,y,b-a+1,color);
    }
    // Lower half. Skip entirely for a flat-bottom triangle (y1==y2): dy12 would
    // be 0 (division by zero) and the upper-half loop above, run with last=y1,
    // has already filled the y1 scanline.
    if (dy12 != 0) {
        sa=0; sb=(int32_t)dx02*(y1-y0);
        for (int16_t y=y1;y<=y2;y++) {
            int16_t a=x1+sa/dy12,b=x0+sb/dy02; sa+=dx12; sb+=dx02;
            if (a>b) std::swap(a,b);
            writeFastHLine(a,y,b-a+1,color);
        }
    }
    endWrite();
}

// Bitmaps
void LinuxGFX::drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                           int16_t w, int16_t h, uint32_t color) {
    int16_t bw=(w+7)/8; uint8_t b=0;
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            if (i&7) b<<=1; else b=bitmap[j*bw+i/8];
            if (b&0x80) writePixel(x+i,y,color);
        }
    endWrite();
}

void LinuxGFX::drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                           int16_t w, int16_t h, uint32_t color, uint32_t bg) {
    int16_t bw=(w+7)/8; uint8_t b=0;
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            if (i&7) b<<=1; else b=bitmap[j*bw+i/8];
            uint32_t px=(b&0x80)?color:bg;
            if ((px>>24)!=0) writePixel(x+i,y,px);
        }
    endWrite();
}

void LinuxGFX::drawBitmap(int16_t x,int16_t y,uint8_t *bitmap,int16_t w,int16_t h,uint32_t color)
{ drawBitmap(x,y,(const uint8_t*)bitmap,w,h,color); }
void LinuxGFX::drawBitmap(int16_t x,int16_t y,uint8_t *bitmap,int16_t w,int16_t h,uint32_t color,uint32_t bg)
{ drawBitmap(x,y,(const uint8_t*)bitmap,w,h,color,bg); }

void LinuxGFX::drawXBitmap(int16_t x, int16_t y, const uint8_t bitmap[],
                             int16_t w, int16_t h, uint32_t color) {
    int16_t bw=(w+7)/8; uint8_t b=0;
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            if (i&7) b>>=1; else b=bitmap[j*bw+i/8];
            if (b&0x01) writePixel(x+i,y,color);
        }
    endWrite();
}

void LinuxGFX::drawGrayscaleBitmap(int16_t x, int16_t y,
                                    const uint8_t bitmap[], int16_t w, int16_t h) {
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            uint8_t v = bitmap[j*w+i];
            writePixel(x+i, y, 0xFF000000u | ((uint32_t)v<<16) | ((uint32_t)v<<8) | v);
        }
    endWrite();
}
void LinuxGFX::drawGrayscaleBitmap(int16_t x,int16_t y,uint8_t *bitmap,int16_t w,int16_t h)
{ drawGrayscaleBitmap(x,y,(const uint8_t*)bitmap,w,h); }

void LinuxGFX::drawGrayscaleBitmap(int16_t x, int16_t y,
                                    const uint8_t bitmap[], const uint8_t mask[],
                                    int16_t w, int16_t h) {
    int16_t bw=(w+7)/8; uint8_t b=0;
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            if (i&7) b<<=1; else b=mask[j*bw+i/8];
            if (b&0x80) {
                uint8_t v=bitmap[j*w+i];
                writePixel(x+i,y, 0xFF000000u|((uint32_t)v<<16)|((uint32_t)v<<8)|v);
            }
        }
    endWrite();
}
void LinuxGFX::drawGrayscaleBitmap(int16_t x,int16_t y,uint8_t *bitmap,uint8_t *mask,int16_t w,int16_t h)
{ drawGrayscaleBitmap(x,y,(const uint8_t*)bitmap,(const uint8_t*)mask,w,h); }

void LinuxGFX::drawRGBBitmap(int16_t x, int16_t y,
                              const uint32_t bitmap[], const uint8_t mask[],
                              int16_t w, int16_t h) {
    int16_t bw=(w+7)/8; uint8_t b=0;
    startWrite();
    for (int16_t j=0;j<h;j++,y++)
        for (int16_t i=0;i<w;i++) {
            if (i&7) b<<=1; else b=mask[j*bw+i/8];
            if (b&0x80) writePixel(x+i,y,bitmap[j*w+i]);
        }
    endWrite();
}
void LinuxGFX::drawRGBBitmap(int16_t x,int16_t y,uint32_t *bitmap,uint8_t *mask,int16_t w,int16_t h)
{ drawRGBBitmap(x,y,(const uint32_t*)bitmap,(const uint8_t*)mask,w,h); }

void LinuxGFX::drawRGB565Bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    if (!m_pBuffer || !bitmap) return;
    int16_t x0 = x, y0 = y, x1 = x+w, y1 = y+h, bx = 0, by = 0;
    if (x0 < 0) { bx = -x0; x0 = 0; }
    if (y0 < 0) { by = -y0; y0 = 0; }
    if (x1 > m_width)  x1 = m_width;
    if (y1 > m_height) y1 = m_height;
    int16_t dw = x1-x0, dh = y1-y0;
    if (dw <= 0 || dh <= 0) return;

#ifdef GFX_USE_DRM_DUMB
    uint32_t dstStride = m_drm[m_drawIdx].pitch / 4;
#else
    uint32_t dstStride = m_pitch / 4;
#endif
    uint32_t       *dst = m_pBuffer + (uint32_t)y0 * dstStride + (uint32_t)x0;
    const uint16_t *src = bitmap    + (uint32_t)by * (uint32_t)w + (uint32_t)bx;

    for (int16_t j = 0; j < dh; j++, dst += dstStride, src += w) {
        for (int16_t i = 0; i < dw; i++) {
            uint16_t c = src[i];
            uint8_t r = ((c >> 11) & 0x1F); r = (r << 3) | (r >> 2);
            uint8_t g = ((c >>  5) & 0x3F); g = (g << 2) | (g >> 4);
            uint8_t b = ( c        & 0x1F); b = (b << 3) | (b >> 2);
            dst[i] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
        }
    }
}

void LinuxGFX::drawBitmapTriangle(
        int16_t x0, int16_t y0, int16_t x1, int16_t y1,
        int16_t x2, int16_t y2,
        const uint32_t *bitmap, int16_t texW, int16_t texH) {
    if (!bitmap || texW <= 0 || texH <= 0) return;

    // UV: p0→(0,0), p1→(texW-1,0), p2→(0,texH-1)
    float u0=0, v0=0, u1=(float)(texW-1), v1=0, u2=0, v2=(float)(texH-1);

    if (y0>y1){std::swap(y0,y1);std::swap(x0,x1);std::swap(u0,u1);std::swap(v0,v1);}
    if (y1>y2){std::swap(y1,y2);std::swap(x1,x2);std::swap(u1,u2);std::swap(v1,v2);}
    if (y0>y1){std::swap(y0,y1);std::swap(x0,x1);std::swap(u0,u1);std::swap(v0,v1);}
    if (y0==y2) return;

    float dy02=(float)(y2-y0);
    startWrite();

    // Top half: y0..y1 — long edge (0→2) and short-top edge (0→1)
    if (y1 > y0) {
        float dy01=(float)(y1-y0);
        float dxA=(x2-x0)/dy02, duA=(u2-u0)/dy02, dvA=(v2-v0)/dy02;
        float dxB=(x1-x0)/dy01, duB=(u1-u0)/dy01, dvB=(v1-v0)/dy01;
        float xA=(float)x0, xB=(float)x0;
        float uA=u0, uB=u0, vA=v0, vB=v0;
        for (int16_t y=y0; y<=y1; y++) {
            int16_t xa=(int16_t)xA, xb=(int16_t)xB;
            float ua=uA, ub=uB, va=vA, vb=vB;
            if (xa>xb){std::swap(xa,xb);std::swap(ua,ub);std::swap(va,vb);}
            float span=(float)(xb-xa);
            for (int16_t x=xa; x<=xb; x++) {
                float t=(span>0.0f)?(float)(x-xa)/span:0.0f;
                int tx=(int)(ua+t*(ub-ua)+0.5f);
                int ty=(int)(va+t*(vb-va)+0.5f);
                if (tx<0)tx=0; if(tx>=texW)tx=texW-1;
                if (ty<0)ty=0; if(ty>=texH)ty=texH-1;
                writePixel(x,y,bitmap[(int32_t)ty*texW+tx]);
            }
            xA+=dxA; uA+=duA; vA+=dvA;
            xB+=dxB; uB+=duB; vB+=dvB;
        }
    }

    // Bottom half: y1..y2 — long edge (0→2) and short-bottom edge (1→2)
    if (y2 > y1) {
        float dy12=(float)(y2-y1);
        float dxA=(x2-x0)/dy02, duA=(u2-u0)/dy02, dvA=(v2-v0)/dy02;
        float dxB=(x2-x1)/dy12, duB=(u2-u1)/dy12, dvB=(v2-v1)/dy12;
        float t01=(float)(y1-y0)/dy02;
        float xA=(float)x0+t01*(x2-x0);
        float uA=u0+t01*(u2-u0), vA=v0+t01*(v2-v0);
        float xB=(float)x1, uB=u1, vB=v1;
        for (int16_t y=y1; y<=y2; y++) {
            int16_t xa=(int16_t)xA, xb=(int16_t)xB;
            float ua=uA, ub=uB, va=vA, vb=vB;
            if (xa>xb){std::swap(xa,xb);std::swap(ua,ub);std::swap(va,vb);}
            float span=(float)(xb-xa);
            for (int16_t x=xa; x<=xb; x++) {
                float t=(span>0.0f)?(float)(x-xa)/span:0.0f;
                int tx=(int)(ua+t*(ub-ua)+0.5f);
                int ty=(int)(va+t*(vb-va)+0.5f);
                if (tx<0)tx=0; if(tx>=texW)tx=texW-1;
                if (ty<0)ty=0; if(ty>=texH)ty=texH-1;
                writePixel(x,y,bitmap[(int32_t)ty*texW+tx]);
            }
            xA+=dxA; uA+=duA; vA+=dvA;
            xB+=dxB; uB+=duB; vB+=dvB;
        }
    }

    endWrite();
}

void LinuxGFX::drawBitmapTriangle(
        int16_t x0, int16_t y0, int16_t x1, int16_t y1,
        int16_t x2, int16_t y2,
        uint32_t *bitmap, int16_t texW, int16_t texH) {
    drawBitmapTriangle(x0,y0,x1,y1,x2,y2,(const uint32_t*)bitmap,texW,texH);
}

//  Default 5×8 font
static const uint8_t s_font[] = {
    0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
    0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
    0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, 0x00,0x1C,0x22,0x41,0x00,
    0x00,0x41,0x22,0x1C,0x00, 0x14,0x08,0x3E,0x08,0x14, 0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00,
    0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
    0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, 0x18,0x14,0x12,0x7F,0x10,
    0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00,
    0x00,0x56,0x36,0x00,0x00, 0x08,0x14,0x22,0x41,0x00, 0x14,0x14,0x14,0x14,0x14,
    0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x51,0x09,0x06, 0x32,0x49,0x79,0x41,0x3E,
    0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01,
    0x3E,0x41,0x49,0x49,0x7A, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
    0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
    0x7F,0x02,0x0C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
    0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F,
    0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F, 0x63,0x14,0x08,0x14,0x63,
    0x07,0x08,0x70,0x08,0x07, 0x61,0x51,0x49,0x45,0x43, 0x00,0x7F,0x41,0x41,0x00,
    0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x7F,0x00, 0x04,0x02,0x01,0x02,0x04,
    0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x04,0x00, 0x20,0x54,0x54,0x54,0x78,
    0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x20, 0x38,0x44,0x44,0x48,0x7F,
    0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x0C,0x52,0x52,0x52,0x3E,
    0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00,
    0x7F,0x10,0x28,0x44,0x00, 0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x18,0x04,0x78,
    0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08,
    0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x20,
    0x04,0x3F,0x44,0x40,0x20, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C,
    0x3C,0x40,0x30,0x40,0x3C, 0x44,0x28,0x10,0x28,0x44, 0x0C,0x50,0x50,0x50,0x3C,
    0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00, 0x00,0x00,0x7F,0x00,0x00,
    0x00,0x41,0x36,0x08,0x00, 0x10,0x08,0x08,0x10,0x08, 0x78,0x46,0x41,0x46,0x78,
};

//  Text rendering
void LinuxGFX::drawChar(int16_t x, int16_t y, unsigned char c,
                         uint32_t color, uint32_t bg, uint8_t sx, uint8_t sy) {
    bool transBg = ((bg >> 24) == 0);

    if (!m_pFont) {
        if (x >= m_width || y >= m_height || (x + 6*sx - 1) < 0 || (y + 8*sy - 1) < 0) return;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = s_font + (c - 32) * 5;
        startWrite();
        for (int8_t col = 0; col < 5; col++) {
            uint8_t bits = glyph[col];
            for (int8_t row = 0; row < 8; row++, bits >>= 1) {
                if (bits & 1) {
                    if (sx==1&&sy==1) writePixel(x+col, y+row, color);
                    else writeFillRect(x+col*sx, y+row*sy, sx, sy, color);
                } else if (!transBg) {
                    if (sx==1&&sy==1) writePixel(x+col, y+row, bg);
                    else writeFillRect(x+col*sx, y+row*sy, sx, sy, bg);
                }
            }
        }
        if (!transBg) {
            for (int8_t row = 0; row < 8; row++) {
                if (sx==1&&sy==1) writePixel(x+5, y+row, bg);
                else writeFillRect(x+5*sx, y+row*sy, sx, sy, bg);
            }
        }
        endWrite();
    } else {
        if (c < m_pFont->first || c > m_pFont->last) return;
        uint8_t ci = c - m_pFont->first;
        const GFXglyph *gl   = &m_pFont->glyph[ci];
        const uint8_t  *bits = m_pFont->bitmap + gl->bitmapOffset;
        int16_t gx = x + gl->xOffset, gy = y + gl->yOffset;
        int16_t gw = gl->width, gh = gl->height;
        uint8_t bit = 0, bits8 = 0;
        startWrite();
        for (int16_t gy2 = 0; gy2 < gh; gy2++)
            for (int16_t gx2 = 0; gx2 < gw; gx2++) {
                if (!(bit++ & 7)) bits8 = *bits++;
                if (bits8 & 0x80) {
                    if (sx==1&&sy==1) writePixel(gx+gx2, gy+gy2, color);
                    else writeFillRect(gx+gx2*sx, gy+gy2*sy, sx, sy, color);
                }
                bits8 <<= 1;
            }
        endWrite();
    }
}

void LinuxGFX::drawChar(int16_t x, int16_t y, unsigned char c,
                         uint32_t color, uint32_t bg, uint8_t size) {
    drawChar(x, y, c, color, bg, size, size);
}

void LinuxGFX::writeText(const char *text) {
    while (*text) {
        unsigned char c = *text++;
        if (c == '\n') {
            m_cursorX = 0;
            m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
        } else if (c != '\r') {
            int16_t adv = m_pFont ? m_pFont->glyph[c - m_pFont->first].xAdvance : 6;
            if (m_textWrap && (m_cursorX + m_textSizeX * adv > m_width)) {
                m_cursorX = 0;
                m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
            }
            drawChar(m_cursorX, m_cursorY, c, m_textColor, m_textBgColor,
                     m_textSizeX, m_textSizeY);
            m_cursorX += m_textSizeX * adv;
        }
    }
}

void LinuxGFX::writeTextF(const char *fmt, ...) {
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    int size = vsnprintf(nullptr, 0, fmt, args);
    if (size < 0) { va_end(args); va_end(args_copy); return; }
    std::vector<char> buf(size + 1);
    vsnprintf(buf.data(), buf.size(), fmt, args_copy);
    va_end(args); va_end(args_copy);
    writeText(buf.data());
}

void LinuxGFX::print(const char *text) {
    writeText(text);
}

void LinuxGFX::println(const char *text) {
    writeText(text);
    m_cursorX = 0;
    m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
}

void LinuxGFX::println() {
    m_cursorX = 0;
    m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
}

// Helper function to rotate a point around an origin
static void rotatePoint(int16_t& x, int16_t& y, int16_t originX, int16_t originY, uint8_t rotation) {
    if (rotation == 0) return;
    
    // Translate to origin
    int16_t dx = x - originX;
    int16_t dy = y - originY;
    
    // Apply rotation (simplified for 90-degree increments)
    int16_t newDx = dx, newDy = dy;
    switch (rotation % 360) {
        case 90:
            newDx = dy;
            newDy = -dx;
            break;
        case 180:
            newDx = -dx;
            newDy = -dy;
            break;
        case 270:
            newDx = -dy;
            newDy = dx;
            break;
    }
    
    // Translate back
    x = originX + newDx;
    y = originY + newDy;
}

// Write text with rotation support
void LinuxGFX::writeTextRotated(const char *text, int16_t originX, int16_t originY) {
    int16_t startX = m_cursorX;
    int16_t startY = m_cursorY;
    
    while (*text) {
        unsigned char c = *text++;
        if (c == '\n') {
            m_cursorX = startX;
            m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
        } else if (c != '\r') {
            int16_t adv = m_pFont ? m_pFont->glyph[c - m_pFont->first].xAdvance : 6;
            if (m_textWrap && (m_cursorX + m_textSizeX * adv > m_width)) {
                m_cursorX = startX;
                m_cursorY += m_textSizeY * (m_pFont ? m_pFont->yAdvance : 8);
            }
            
            int16_t charX = m_cursorX;
            int16_t charY = m_cursorY;
            rotatePoint(charX, charY, originX, originY, m_textRotation);
            
            drawChar(charX, charY, c, m_textColor, m_textBgColor,
                     m_textSizeX, m_textSizeY);
            m_cursorX += m_textSizeX * adv;
        }
    }
}

// All-in-one text drawing function
void LinuxGFX::setText(int16_t x, int16_t y, const char *text, uint32_t color, 
                       uint32_t bgColor, uint8_t sizeX, uint8_t sizeY, const GFXfont *font,
                       uint8_t rotation) {
    setCursor(x, y);
    setTextColor(color, bgColor);
    setTextSize(sizeX, sizeY);
    setFont(font);
    setTextRotation(rotation);
    
    if (rotation == 0) {
        writeText(text);
    } else {
        writeTextRotated(text, x, y);
    }
}

void LinuxGFX::setTextRotation(uint8_t rotation) {
    m_textRotation = rotation % 360;
}

uint8_t LinuxGFX::getTextRotation() const {
    return m_textRotation;
}


void LinuxGFX::setFont     (const GFXfont *f)         { m_pFont = f; }
void LinuxGFX::setCursor   (int16_t x, int16_t y)     { m_cursorX = x; m_cursorY = y; }
void LinuxGFX::setTextColor(uint32_t c)                { m_textColor = c; m_textBgColor = GFX_TRANSPARENT; }
void LinuxGFX::setTextColor(uint32_t c, uint32_t bg)   { m_textColor = c; m_textBgColor = bg; }
void LinuxGFX::setTextColorTransparentBg(uint32_t c)   { m_textColor = c; m_textBgColor = GFX_TRANSPARENT; }
void LinuxGFX::setTextSize (uint8_t s)                 { m_textSizeX = m_textSizeY = s ? s : 1; }
void LinuxGFX::setTextSize (uint8_t sx, uint8_t sy)    { m_textSizeX = sx?sx:1; m_textSizeY = sy?sy:1; }
void LinuxGFX::setTextWrap (bool w)                    { m_textWrap = w; }

// Control & dimensions
void LinuxGFX::setRotation(uint8_t r) {
    m_rotation = r % 4;
    if (m_rotation == 1 || m_rotation == 3) std::swap(m_width, m_height);
}
uint8_t  LinuxGFX::getRotation()  const { return m_rotation; }
void     LinuxGFX::invertDisplay(bool i) { m_inverted = i; }
int16_t  LinuxGFX::width()        const { return m_width; }
int16_t  LinuxGFX::height()       const { return m_height; }
int16_t  LinuxGFX::getCursorX()   const { return m_cursorX; }
int16_t  LinuxGFX::getCursorY()   const { return m_cursorY; }

void LinuxGFX::drawFastVLineInternal(int16_t x, int16_t y, int16_t h, uint32_t c) { writeFastVLine(x,y,h,c); }
void LinuxGFX::drawFastHLineInternal(int16_t x, int16_t y, int16_t w, uint32_t c) { writeFastHLine(x,y,w,c); }

//  Color helpers

// Backwards-compatible: color565(r,g,b) now returns full-precision opaque ARGB8888.
// The name "color565" is kept so old code compiles without changes.
uint32_t LinuxGFX::color565(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t LinuxGFX::color565(uint32_t rgb) {
    // Accept 0xRRGGBB — return opaque ARGB8888
    return 0xFF000000u | (rgb & 0x00FFFFFFu);
}

uint32_t LinuxGFX::color888(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t LinuxGFX::colorRGB(uint8_t r, uint8_t g, uint8_t b) {
    return color888(r, g, b);
}

uint32_t LinuxGFX::colorARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t LinuxGFX::fromRGB565(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) << 3;   // 5-bit → 8-bit
    uint8_t g = ((c >>  5) & 0x3F) << 2;   // 6-bit → 8-bit
    uint8_t b = ( c        & 0x1F) << 3;   // 5-bit → 8-bit
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// =============================================================================
//  Draw style API
// =============================================================================

void LinuxGFX::setDrawStyle  (const GFXDrawStyle &s) { m_drawStyle = s; }
const GFXDrawStyle &LinuxGFX::getDrawStyle()   const { return m_drawStyle; }
void LinuxGFX::setStrokeWidth(uint8_t w)             { m_drawStyle.strokeWidth = w; }
void LinuxGFX::setLineCap    (GFXLineCap c)          { m_drawStyle.lineCap     = c; }
void LinuxGFX::setLineJoin   (GFXLineJoin j)         { m_drawStyle.lineJoin    = j; }
void LinuxGFX::setAntiAlias  (bool e)                { m_drawStyle.antiAlias   = e; }

// =============================================================================
//  Styled rendering helpers
// =============================================================================

// -----------------------------------------------------------------------------
// writeLineAA — Xiaolin Wu anti-aliased line (strokeWidth == 1).
// Alpha of the supplied color is treated as 100 % coverage; fractional coverage
// is applied on top of it so semi-transparent AA lines work correctly.
// -----------------------------------------------------------------------------
void LinuxGFX::writeLineAA(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color) {
    uint32_t baseAlpha = (color >> 24) & 0xFF;
    uint32_t rgb       = color & 0x00FFFFFFu;

    // plot helper: write a pixel with fractional brightness applied to alpha
    auto plot = [&](int x, int y, float brightness) {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
        uint32_t a = (uint32_t)(baseAlpha * brightness + 0.5f);
        if (a == 0) return;
        writePixel((int16_t)x, (int16_t)y, rgb | (a << 24));
    };

    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep)  { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1){ std::swap(x0, x1); std::swap(y0, y1); }

    float dx       = (float)(x1 - x0);
    float dy       = (float)(y1 - y0);
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;

    // --- first endpoint ---
    float xend  = (float)(int)(x0 + 0.5f);
    float yend  = y0 + gradient * (xend - x0);
    float xgap  = 1.0f - ((x0 + 0.5f) - floorf(x0 + 0.5f));
    int   xpx1  = (int)xend;
    int   ypx1  = (int)floorf(yend);
    float fp1   = yend - floorf(yend);
    if (steep) { plot(ypx1,   xpx1, (1.0f - fp1) * xgap);
                 plot(ypx1+1, xpx1, fp1 * xgap); }
    else       { plot(xpx1,   ypx1, (1.0f - fp1) * xgap);
                 plot(xpx1, ypx1+1, fp1 * xgap); }
    float intery = yend + gradient;

    // --- second endpoint ---
    xend = (float)(int)(x1 + 0.5f);
    yend = y1 + gradient * (xend - x1);
    xgap = (x1 + 0.5f) - floorf(x1 + 0.5f);
    int xpx2 = (int)xend;
    int ypx2 = (int)floorf(yend);
    float fp2 = yend - floorf(yend);
    if (steep) { plot(ypx2,   xpx2, (1.0f - fp2) * xgap);
                 plot(ypx2+1, xpx2, fp2 * xgap); }
    else       { plot(xpx2,   ypx2, (1.0f - fp2) * xgap);
                 plot(xpx2, ypx2+1, fp2 * xgap); }

    // --- inner span ---
    if (steep) {
        for (int x = xpx1 + 1; x < xpx2; ++x) {
            float frac = intery - floorf(intery);
            plot((int)floorf(intery),   x, 1.0f - frac);
            plot((int)floorf(intery)+1, x, frac);
            intery += gradient;
        }
    } else {
        for (int x = xpx1 + 1; x < xpx2; ++x) {
            float frac = intery - floorf(intery);
            plot(x, (int)floorf(intery),   1.0f - frac);
            plot(x, (int)floorf(intery)+1, frac);
            intery += gradient;
        }
    }
}

// -----------------------------------------------------------------------------
// writeThickLine — filled parallelogram + optional end caps.
// -----------------------------------------------------------------------------
void LinuxGFX::writeThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               uint32_t color, uint8_t width) {
    if (width <= 1) { writeLine(x0, y0, x1, y1, color); return; }

    float fdx = (float)(x1 - x0);
    float fdy = (float)(y1 - y0);
    float len = sqrtf(fdx*fdx + fdy*fdy);

    if (len < 1.0f) {
        int16_t r = (int16_t)(width / 2);
        if (m_drawStyle.lineCap == GFXLineCap::Round)
            fillCircle(x0, y0, r, color);
        else
            writeFillRect((int16_t)(x0 - r), (int16_t)(y0 - r), (int16_t)width, (int16_t)width, color);
        return;
    }

    // Perpendicular half-width offsets
    float px = -fdy / len * (float)width * 0.5f;
    float py =  fdx / len * (float)width * 0.5f;

    // Endpoint offsets for Square cap
    float ex0 = (float)x0, ey0 = (float)y0;
    float ex1 = (float)x1, ey1 = (float)y1;
    if (m_drawStyle.lineCap == GFXLineCap::Square) {
        float ndx = fdx / len * (float)width * 0.5f;
        float ndy = fdy / len * (float)width * 0.5f;
        ex0 -= ndx; ey0 -= ndy;
        ex1 += ndx; ey1 += ndy;
    }

    // 4 corners of the stroke parallelogram
    int16_t ax = (int16_t)(ex0 - px), ay = (int16_t)(ey0 - py);
    int16_t bx = (int16_t)(ex0 + px), by = (int16_t)(ey0 + py);
    int16_t cx = (int16_t)(ex1 + px), cy = (int16_t)(ey1 + py);
    int16_t ddx = (int16_t)(ex1 - px), ddy2 = (int16_t)(ey1 - py);

    fillTriangle(ax, ay, bx, by, cx, cy, color);
    fillTriangle(ax, ay, cx, cy, ddx, ddy2, color);

    if (m_drawStyle.lineCap == GFXLineCap::Round) {
        fillCircle(x0, y0, (int16_t)(width / 2), color);
        fillCircle(x1, y1, (int16_t)(width / 2), color);
    }
}

// -----------------------------------------------------------------------------
// writeCircleAA — 1-pixel-wide anti-aliased circle outline.
// Only the ±1 pixel band around the ideal radius is scanned.
// -----------------------------------------------------------------------------
void LinuxGFX::writeCircleAA(int16_t x0, int16_t y0, int16_t r, uint32_t color) {
    uint32_t baseAlpha = (color >> 24) & 0xFF;
    uint32_t rgb       = color & 0x00FFFFFFu;
    float    fr        = (float)r;

    for (int16_t dy = (int16_t)(-(r + 1)); dy <= (int16_t)(r + 1); ++dy) {
        float dy2    = (float)dy * (float)dy;
        float outer2 = (fr + 1.0f) * (fr + 1.0f) - dy2;
        float inner2 = (fr - 1.0f) * (fr - 1.0f) - dy2;
        if (outer2 < 0.0f) continue;
        int ox = (int)sqrtf(outer2);
        int ix = (inner2 > 0.0f) ? (int)sqrtf(inner2) : 0;
        for (int dx = ix; dx <= ox; ++dx) {
            float dist    = sqrtf((float)(dx * dx) + dy2) - fr;
            float alpha_f = 1.0f - fabsf(dist);
            if (alpha_f <= 0.0f) continue;
            uint32_t a = (uint32_t)(baseAlpha * alpha_f + 0.5f);
            if (a == 0) continue;
            uint32_t c = rgb | (a << 24);
            writePixel((int16_t)(x0 + dx), (int16_t)(y0 + dy), c);
            if (dx > 0)
                writePixel((int16_t)(x0 - dx), (int16_t)(y0 + dy), c);
        }
    }
}

// -----------------------------------------------------------------------------
// writeThickCircle — fills the annular ring from r-w/2 to r+w/2.
// -----------------------------------------------------------------------------
void LinuxGFX::writeThickCircle(int16_t x0, int16_t y0, int16_t r,
                                 uint32_t color, uint8_t w) {
    int16_t r_outer = (int16_t)(r + (int16_t)(w / 2));
    int16_t r_inner = (int16_t)(r - (int16_t)((w + 1) / 2));
    if (r_inner < 0) r_inner = 0;

    int32_t ro2 = (int32_t)r_outer * r_outer;
    int32_t ri2 = (int32_t)r_inner * r_inner;

    for (int16_t dy = -r_outer; dy <= r_outer; ++dy) {
        int32_t dy2 = (int32_t)dy * dy;
        if (dy2 > ro2) continue;

        int16_t ox = (int16_t)sqrtf((float)(ro2 - dy2));

        if (dy2 < ri2) {
            // Ring: two bands, excluding the hollow interior
            int16_t ix  = (int16_t)(sqrtf((float)(ri2 - dy2)) + 0.5f);
            int16_t len = (int16_t)(ox - ix + 1);
            if (len > 0) {
                writeFastHLine((int16_t)(x0 - ox), (int16_t)(y0 + dy), len, color);
                writeFastHLine((int16_t)(x0 + ix), (int16_t)(y0 + dy), len, color);
            }
        } else {
            // Full span — we're at the top/bottom dome of the ring
            writeFastHLine((int16_t)(x0 - ox), (int16_t)(y0 + dy), (int16_t)(2 * ox + 1), color);
        }
    }
}

// -----------------------------------------------------------------------------
// writeThickArc — thick arc restricted to selected quadrants.
// cornermask: 0x1=top-left  0x2=top-right  0x4=bottom-right  0x8=bottom-left
// Used internally by drawRoundRect for thick-stroked corners.
// -----------------------------------------------------------------------------
void LinuxGFX::writeThickArc(int16_t x0, int16_t y0, int16_t r, uint8_t cornermask,
                              uint32_t color, uint8_t w) {
    int16_t r_outer = (int16_t)(r + (int16_t)(w / 2));
    int16_t r_inner = (int16_t)(r - (int16_t)((w + 1) / 2));
    if (r_inner < 0) r_inner = 0;

    int32_t ro2 = (int32_t)r_outer * r_outer;
    int32_t ri2 = (int32_t)r_inner * r_inner;

    for (int16_t dy = -r_outer; dy <= r_outer; ++dy) {
        // Determine which horizontal sides are needed for this row
        bool draw_left, draw_right;
        if (dy < 0) {
            draw_left  = (cornermask & 0x1) != 0; // top-left
            draw_right = (cornermask & 0x2) != 0; // top-right
        } else if (dy > 0) {
            draw_left  = (cornermask & 0x8) != 0; // bottom-left
            draw_right = (cornermask & 0x4) != 0; // bottom-right
        } else {
            // dy == 0: boundary row shared by upper/lower corners
            draw_left  = (cornermask & (0x1u | 0x8u)) != 0;
            draw_right = (cornermask & (0x2u | 0x4u)) != 0;
        }
        if (!draw_left && !draw_right) continue;

        int32_t dy2 = (int32_t)dy * dy;
        if (dy2 > ro2) continue;

        int16_t ox = (int16_t)sqrtf((float)(ro2 - dy2));

        if (dy2 < ri2) {
            int16_t ix  = (int16_t)(sqrtf((float)(ri2 - dy2)) + 0.5f);
            int16_t len = (int16_t)(ox - ix + 1);
            if (len > 0) {
                if (draw_left)  writeFastHLine((int16_t)(x0 - ox), (int16_t)(y0 + dy), len, color);
                if (draw_right) writeFastHLine((int16_t)(x0 + ix), (int16_t)(y0 + dy), len, color);
            }
        } else {
            int16_t span = (int16_t)(ox + 1);
            if (draw_left)  writeFastHLine((int16_t)(x0 - ox), (int16_t)(y0 + dy), span, color);
            if (draw_right) writeFastHLine(x0,                 (int16_t)(y0 + dy), span, color);
        }
    }
}

// -----------------------------------------------------------------------------
// writeArcThick — thick arc outline over an arbitrary angular span.
// Scans the bounding box, keeping pixels inside the radial band
// [r_inner, r_outer] AND inside the clockwise span [startAngle, startAngle+sweep].
// -----------------------------------------------------------------------------
void LinuxGFX::writeArcThick(int16_t x0, int16_t y0, int16_t r,
                              float startAngle, float sweep,
                              uint32_t color, uint8_t w) {
    int16_t r_outer = (int16_t)(r + (int16_t)(w / 2));
    int16_t r_inner = (int16_t)(r - (int16_t)((w + 1) / 2));
    if (r_inner < 0) r_inner = 0;

    int32_t ro2 = (int32_t)r_outer * r_outer;
    int32_t ri2 = (int32_t)r_inner * r_inner;

    const float R2D  = 57.29577951308232f;
    const bool  full = (sweep >= 359.9999f);
    const float endN = startAngle + sweep;
    auto wrap180 = [](float a) -> float {
        float t = fmodf(a + 180.0f, 360.0f);
        if (t < 0.0f) t += 360.0f;
        return t - 180.0f;
    };

    for (int16_t dy = -r_outer; dy <= r_outer; ++dy) {
        int32_t dy2 = (int32_t)dy * dy;
        if (dy2 > ro2) continue;
        for (int16_t dx = -r_outer; dx <= r_outer; ++dx) {
            int32_t d2 = (int32_t)dx * dx + dy2;
            if (d2 > ro2 || d2 < ri2) continue;
            if (!full) {
                float pa = atan2f((float)dy, (float)dx) * R2D;
                if (wrap180(pa - startAngle) < 0.0f) continue;
                if (wrap180(endN - pa)       < 0.0f) continue;
            }
            writePixel((int16_t)(x0 + dx), (int16_t)(y0 + dy), color);
        }
    }
}

// -----------------------------------------------------------------------------
// writeArcAA — anti-aliased 1-pixel arc outline.
// Coverage combines a radial distance-field (|d - r|) with tangential feathering
// at the two angular endpoints, so the end caps are smoothed as well.
// -----------------------------------------------------------------------------
void LinuxGFX::writeArcAA(int16_t x0, int16_t y0, int16_t r,
                           float startAngle, float sweep, uint32_t color) {
    uint32_t baseAlpha = (color >> 24) & 0xFF;
    uint32_t rgb       = color & 0x00FFFFFFu;
    float    fr        = (float)r;

    const float R2D  = 57.29577951308232f;
    const float D2R  = 0.01745329251994330f;
    const bool  full = (sweep >= 359.9999f);
    const float endN = startAngle + sweep;
    auto wrap180 = [](float a) -> float {
        float t = fmodf(a + 180.0f, 360.0f);
        if (t < 0.0f) t += 360.0f;
        return t - 180.0f;
    };

    for (int16_t dy = -(int16_t)(r + 1); dy <= (int16_t)(r + 1); ++dy) {
        float dy2    = (float)dy * (float)dy;
        float outer2 = (fr + 1.0f) * (fr + 1.0f) - dy2;
        if (outer2 < 0.0f) continue;
        int ox = (int)sqrtf(outer2);
        for (int dx = -ox; dx <= ox; ++dx) {
            float d         = sqrtf((float)dx * (float)dx + dy2);
            float radialCov = 1.0f - fabsf(d - fr);   // width-1 band
            if (radialCov <= 0.0f) continue;

            float cov = radialCov;
            if (!full) {
                float pa        = atan2f((float)dy, (float)dx) * R2D;
                float fromStart = wrap180(pa - startAngle);
                float toEnd     = wrap180(endN - pa);
                float edgeDeg   = (fromStart < toEnd) ? fromStart : toEnd;
                // tangential distance (px) = edgeAngle(rad) * radius; feather ~1px.
                float angCov    = edgeDeg * D2R * (d > 0.5f ? d : 0.5f) + 0.5f;
                if (angCov <= 0.0f) continue;
                if (angCov < cov) cov = angCov;
            }
            if (cov > 1.0f) cov = 1.0f;

            uint32_t a = (uint32_t)(baseAlpha * cov + 0.5f);
            if (a == 0) continue;
            writePixel((int16_t)(x0 + dx), (int16_t)(y0 + dy), rgb | (a << 24));
        }
    }
}

// =============================================================================
//  SDL Event Handling (SDL back-end only)
// =============================================================================

#ifdef GFXSDL

bool LinuxGFX::processEvents() {
    if (!m_pWindow) return false;
    
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                m_shouldQuit = true;
                return false;
            
            case SDL_MOUSEMOTION:
                m_mouseX = event.motion.x;
                m_mouseY = event.motion.y;
                if (m_eventCallback) {
                    GFXInputEvent inputEvent;
                    inputEvent.type = GFXEventType::MOUSE_MOVE;
                    inputEvent.x = m_mouseX;
                    inputEvent.y = m_mouseY;
                    inputEvent.button = 0;
                    m_eventCallback(inputEvent);
                }
                break;
            
            case SDL_MOUSEBUTTONDOWN:
                if (m_eventCallback) {
                    GFXInputEvent inputEvent;
                    inputEvent.type = GFXEventType::MOUSE_BUTTON_DOWN;
                    inputEvent.x = event.button.x;
                    inputEvent.y = event.button.y;
                    inputEvent.button = event.button.button - 1;  // Convert SDL (1,2,3) to (0,1,2)
                    m_eventCallback(inputEvent);
                }
                break;
            
            case SDL_MOUSEBUTTONUP:
                if (m_eventCallback) {
                    GFXInputEvent inputEvent;
                    inputEvent.type = GFXEventType::MOUSE_BUTTON_UP;
                    inputEvent.x = event.button.x;
                    inputEvent.y = event.button.y;
                    inputEvent.button = event.button.button - 1;  // Convert SDL (1,2,3) to (0,1,2)
                    m_eventCallback(inputEvent);
                }
                break;
            
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                // Key events can be extended here if needed
                break;
            
            default:
                break;
        }
    }
    
    return !m_shouldQuit;
}

void LinuxGFX::setEventCallback(const GFXEventCallback& callback) {
    m_eventCallback = callback;
}

void LinuxGFX::getMousePosition(int16_t& x, int16_t& y) const {
    x = m_mouseX;
    y = m_mouseY;
}

#endif  // GFXSDL

// =============================================================================
//  GFXcanvas implementation
// =============================================================================

// ---------------------------------------------------------------------------
//  LinuxGFX no-device constructor (used only by GFXcanvas).
//  Initialises all common members to safe defaults without opening any fd.
// ---------------------------------------------------------------------------
LinuxGFX::LinuxGFX(CanvasTag) noexcept
    : m_fbFd(-1), m_pFbMem(nullptr), m_fbMemSize(0),
      m_pitch(0), m_depth(32),
      m_bufferCount(0), m_drawBufferIndex(0), m_displayBufferIndex(0),
      m_multiBufferEnabled(false),
#ifdef GFX_USE_DRM
      m_drmFd(-1), m_drmCrtcId(0), m_drmConnId(0),
      m_drmFront(0), m_drmFlipPending(false),
#endif
      m_pBuffer(nullptr),
      m_width(0), m_height(0),
      m_cursorX(0), m_cursorY(0),
      m_textColor(GFX_WHITE), m_textBgColor(GFX_TRANSPARENT),
      m_textSizeX(1), m_textSizeY(1), m_textRotation(0),
      m_textWrap(true), m_rotation(0),
      m_inverted(false), m_inTransaction(false),
      m_pFont(nullptr), m_fontSizeMultiplied(true)
{
    for (int i = 0; i < 3; i++) {
        m_buffers[i].pData  = nullptr;
        m_buffers[i].bOwned = false;
        m_buffers[i].bReady = false;
    }
#ifdef GFX_USE_DRM
    memset(m_drmBufs, 0, sizeof(m_drmBufs));
#endif
#ifdef GFXSDL
    // A canvas owns no SDL backend. These MUST be null so ~LinuxGFX/stop() does
    // not try to destroy/free garbage pointers when a GFXcanvas is destroyed.
    m_pWindow        = nullptr;
    m_pRenderer      = nullptr;
    m_pTexture       = nullptr;
    m_pSurfaceBuffer = nullptr;
    m_mouseX         = 0;
    m_mouseY         = 0;
    m_shouldQuit     = false;
#endif
}

// ----------------------------------------------------------------------------
//  Internal helper: override LinuxGFX's device pixel paths so that every
//  inherited draw primitive writes to the canvas heap buffer instead.
//  We intercept via the virtual-like protected hooks used throughout LinuxGFX.
// ----------------------------------------------------------------------------

// ---- Colour conversion helpers ---------------------------------------------

uint32_t GFXcanvas::argbFromGray8(uint8_t g) {
    return 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
}

uint32_t GFXcanvas::argbFromRGB565(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F); r = (r << 3) | (r >> 2);
    uint8_t g = ((c >>  5) & 0x3F); g = (g << 2) | (g >> 4);
    uint8_t b = ( c        & 0x1F); b = (b << 3) | (b >> 2);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t GFXcanvas::argbFromRGB888(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint8_t GFXcanvas::gray8FromARGB(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b =  argb        & 0xFF;
    // BT.601 luma
    return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

uint16_t GFXcanvas::rgb565FromARGB(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b =  argb        & 0xFF;
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)(b         >> 3)));
}

void GFXcanvas::rgb888FromARGB(uint32_t argb, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (argb >> 16) & 0xFF;
    g = (argb >>  8) & 0xFF;
    b =  argb        & 0xFF;
}

// ---- Constructor / destructor ----------------------------------------------

GFXcanvas::GFXcanvas(uint16_t w, uint16_t h, Format fmt, bool allocate_buffer)
    : LinuxGFX(CanvasTag{}),
      m_format(fmt),
      m_canvasBuffer(nullptr),
      m_bufferBytes(0),
      m_bufferOwned(false)
{
    m_width  = (int16_t)w;
    m_height = (int16_t)h;

    // Compute buffer size
    size_t pixels = (size_t)w * (size_t)h;
    switch (fmt) {
        case Format::BW:       m_bufferBytes = (pixels + 7) / 8; break;
        case Format::GRAY8:    m_bufferBytes = pixels;            break;
        case Format::RGB565:   m_bufferBytes = pixels * 2;        break;
        case Format::RGB888:   m_bufferBytes = pixels * 3;        break;
        case Format::ARGB8888: m_bufferBytes = pixels * 4;        break;
    }

    if (allocate_buffer && m_bufferBytes > 0) {
        m_canvasBuffer = (uint8_t*)calloc(1, m_bufferBytes);
        m_bufferOwned  = (m_canvasBuffer != nullptr);
    }

    // Keep m_pBuffer pointing at the canvas buffer so any inherited path that
    // writes through m_pBuffer directly (e.g. fast-H/V line DRM path) is safe.
    // It will only be valid for ARGB8888 format; other formats must always go
    // through setPixelRaw.
    if (fmt == Format::ARGB8888)
        m_pBuffer = (uint32_t*)m_canvasBuffer;
}

GFXcanvas::~GFXcanvas() {
    if (m_bufferOwned && m_canvasBuffer) {
        free(m_canvasBuffer);
        m_canvasBuffer = nullptr;
    }
}

// ---- Buffer management -----------------------------------------------------

bool GFXcanvas::attachBuffer(uint8_t *buf) {
    if (!buf) return false;
    if (m_bufferOwned && m_canvasBuffer) {
        free(m_canvasBuffer);
    }
    m_canvasBuffer = buf;
    m_bufferOwned  = false;
    if (m_format == Format::ARGB8888)
        m_pBuffer = (uint32_t*)m_canvasBuffer;
    return true;
}

void GFXcanvas::detachBuffer() {
    if (m_bufferOwned && m_canvasBuffer) {
        free(m_canvasBuffer);
    }
    m_canvasBuffer = nullptr;
    m_bufferOwned  = false;
    m_pBuffer      = nullptr;
}

uint16_t *GFXcanvas::getBufferRGB565() const {
    if (m_format != Format::RGB565) return nullptr;
    return (uint16_t*)m_canvasBuffer;
}

uint32_t *GFXcanvas::getBufferARGB8888() const {
    if (m_format != Format::ARGB8888) return nullptr;
    return (uint32_t*)m_canvasBuffer;
}

bool GFXcanvas::copyToARGB8888(uint32_t *dst) const {
    if (!m_canvasBuffer || !dst) return false;
    size_t pixels = (size_t)m_width * (size_t)m_height;
    for (size_t i = 0; i < pixels; i++) {
        switch (m_format) {
            case Format::BW: {
                uint8_t bit = (m_canvasBuffer[i / 8] >> (7 - (i & 7))) & 1;
                dst[i] = bit ? GFX_WHITE : GFX_BLACK;
                break;
            }
            case Format::GRAY8:
                dst[i] = argbFromGray8(m_canvasBuffer[i]);
                break;
            case Format::RGB565:
                dst[i] = argbFromRGB565(((uint16_t*)m_canvasBuffer)[i]);
                break;
            case Format::RGB888: {
                uint8_t *p = m_canvasBuffer + i * 3;
                dst[i] = argbFromRGB888(p[0], p[1], p[2]);
                break;
            }
            case Format::ARGB8888:
                dst[i] = ((uint32_t*)m_canvasBuffer)[i];
                break;
        }
    }
    return true;
}

bool GFXcanvas::copyToRGB565(uint16_t *dst) const {
    if (!m_canvasBuffer || !dst) return false;
    size_t pixels = (size_t)m_width * (size_t)m_height;
    for (size_t i = 0; i < pixels; i++) {
        uint32_t argb = 0;
        switch (m_format) {
            case Format::BW: {
                uint8_t bit = (m_canvasBuffer[i / 8] >> (7 - (i & 7))) & 1;
                argb = bit ? GFX_WHITE : GFX_BLACK;
                break;
            }
            case Format::GRAY8:
                argb = argbFromGray8(m_canvasBuffer[i]);
                break;
            case Format::RGB565:
                dst[i] = ((uint16_t*)m_canvasBuffer)[i];
                continue;
            case Format::RGB888: {
                uint8_t *p = m_canvasBuffer + i * 3;
                argb = argbFromRGB888(p[0], p[1], p[2]);
                break;
            }
            case Format::ARGB8888:
                argb = ((uint32_t*)m_canvasBuffer)[i];
                break;
        }
        dst[i] = rgb565FromARGB(argb);
    }
    return true;
}

void GFXcanvas::byteSwap() {
    if (!m_canvasBuffer) return;
    if (m_format == Format::RGB565) {
        uint16_t *p   = (uint16_t*)m_canvasBuffer;
        size_t    n   = (size_t)m_width * (size_t)m_height;
        for (size_t i = 0; i < n; i++)
            p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8));
    }
    // For other formats byteSwap is a no-op (not meaningful / endian-neutral)
}

// ---- Low-level pixel access ------------------------------------------------

void GFXcanvas::setPixelRaw(int16_t x, int16_t y, uint32_t argb) {
    if (!m_canvasBuffer) return;
    // Alpha-blend over existing pixel (same logic as LinuxGFX::blendARGB but
    // the destination is read from the canvas buffer in its native format).
    uint32_t sa = argb >> 24;
    if (sa == 0) return;  // GFX_TRANSPARENT — skip

    uint32_t dst_argb = 0;
    if (sa < 255) dst_argb = getPixelRaw(x, y);
    uint32_t blended = (sa == 255) ? argb : blendARGB(argb, dst_argb);

    size_t offset = (size_t)y * (size_t)m_width + (size_t)x;
    switch (m_format) {
        case Format::BW: {
            uint8_t luma = gray8FromARGB(blended);
            uint8_t bit  = 7 - (offset & 7);
            if (luma >= 128)
                m_canvasBuffer[offset / 8] |=  (uint8_t)(1u << bit);
            else
                m_canvasBuffer[offset / 8] &= ~(uint8_t)(1u << bit);
            break;
        }
        case Format::GRAY8:
            m_canvasBuffer[offset] = gray8FromARGB(blended);
            break;
        case Format::RGB565:
            ((uint16_t*)m_canvasBuffer)[offset] = rgb565FromARGB(blended);
            break;
        case Format::RGB888: {
            uint8_t *p = m_canvasBuffer + offset * 3;
            rgb888FromARGB(blended, p[0], p[1], p[2]);
            break;
        }
        case Format::ARGB8888:
            ((uint32_t*)m_canvasBuffer)[offset] = blended;
            break;
    }
}

uint32_t GFXcanvas::getPixelRaw(int16_t x, int16_t y) const {
    if (!m_canvasBuffer) return 0;
    size_t offset = (size_t)y * (size_t)m_width + (size_t)x;
    switch (m_format) {
        case Format::BW: {
            uint8_t bit = 7 - (offset & 7);
            return ((m_canvasBuffer[offset / 8] >> bit) & 1) ? GFX_WHITE : GFX_BLACK;
        }
        case Format::GRAY8:
            return argbFromGray8(m_canvasBuffer[offset]);
        case Format::RGB565:
            return argbFromRGB565(((uint16_t*)m_canvasBuffer)[offset]);
        case Format::RGB888: {
            const uint8_t *p = m_canvasBuffer + offset * 3;
            return argbFromRGB888(p[0], p[1], p[2]);
        }
        case Format::ARGB8888:
            return ((uint32_t*)m_canvasBuffer)[offset];
    }
    return 0;
}

// ---- Public pixel API (override LinuxGFX) ----------------------------------

void GFXcanvas::drawPixel(int16_t x, int16_t y, uint32_t color) {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
    // Handle rotation the same way LinuxGFX does for consistency
    int16_t t;
    switch (m_rotation) {
        case 1: t = x; x = m_width - 1 - y; y = t;            break;
        case 2: x = m_width - 1 - x; y = m_height - 1 - y;    break;
        case 3: t = x; x = y; y = m_height - 1 - t;           break;
        default: break;
    }
    setPixelRaw(x, y, color);
}

uint32_t GFXcanvas::getPixel(int16_t x, int16_t y) const {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return 0;
    int16_t t;
    switch (m_rotation) {
        case 1: t = x; x = m_width - 1 - y; y = t;            break;
        case 2: x = m_width - 1 - x; y = m_height - 1 - y;    break;
        case 3: t = x; x = y; y = m_height - 1 - t;           break;
        default: break;
    }
    return getPixelRaw(x, y);
}

void GFXcanvas::fillScreen(uint32_t color) {
    if (!m_canvasBuffer) return;
    uint32_t sa = color >> 24;
    if (sa == 0) return;

    size_t pixels = (size_t)m_width * (size_t)m_height;
    switch (m_format) {
        case Format::BW:
            memset(m_canvasBuffer, (gray8FromARGB(color) >= 128) ? 0xFF : 0x00,
                   m_bufferBytes);
            break;
        case Format::GRAY8:
            memset(m_canvasBuffer, gray8FromARGB(color), m_bufferBytes);
            break;
        case Format::RGB565: {
            uint16_t c16 = rgb565FromARGB(color);
            uint16_t *p  = (uint16_t*)m_canvasBuffer;
            for (size_t i = 0; i < pixels; i++) p[i] = c16;
            break;
        }
        case Format::RGB888: {
            uint8_t r, g, b;
            rgb888FromARGB(color, r, g, b);
            uint8_t *p = m_canvasBuffer;
            for (size_t i = 0; i < pixels; i++) { *p++ = r; *p++ = g; *p++ = b; }
            break;
        }
        case Format::ARGB8888: {
            uint32_t *p = (uint32_t*)m_canvasBuffer;
            for (size_t i = 0; i < pixels; i++) p[i] = color;
            break;
        }
    }
}

// Fast horizontal line — bypasses rotation/clip overhead for inner loops
void GFXcanvas::drawFastHLine(int16_t x, int16_t y, int16_t w, uint32_t color) {
    if (!m_canvasBuffer || w <= 0) return;
    // Clip
    if (x < 0) { w += x; x = 0; }
    if (x + w > m_width) w = m_width - x;
    if (y < 0 || y >= m_height || w <= 0) return;

    size_t  row    = (size_t)y * (size_t)m_width;
    uint32_t sa    = color >> 24;
    if (sa == 0) return;

    switch (m_format) {
        case Format::BW:
            for (int16_t i = x; i < x + w; i++) {
                uint8_t luma = gray8FromARGB(color);
                uint8_t bit  = 7 - (((row + i)) & 7);
                size_t  byte = (row + i) / 8;
                if (luma >= 128)
                    m_canvasBuffer[byte] |=  (uint8_t)(1u << bit);
                else
                    m_canvasBuffer[byte] &= ~(uint8_t)(1u << bit);
            }
            break;
        case Format::GRAY8: {
            uint8_t g = gray8FromARGB(color);
            memset(m_canvasBuffer + row + x, g, (size_t)w);
            break;
        }
        case Format::RGB565: {
            uint16_t c16 = rgb565FromARGB(color);
            uint16_t *p  = (uint16_t*)m_canvasBuffer + row + x;
            for (int16_t i = 0; i < w; i++) p[i] = c16;
            break;
        }
        case Format::RGB888: {
            uint8_t r, g, b;
            rgb888FromARGB(color, r, g, b);
            uint8_t *p = m_canvasBuffer + (row + x) * 3;
            for (int16_t i = 0; i < w; i++) { *p++ = r; *p++ = g; *p++ = b; }
            break;
        }
        case Format::ARGB8888: {
            uint32_t *p = (uint32_t*)m_canvasBuffer + row + x;
            for (int16_t i = 0; i < w; i++) p[i] = color;
            break;
        }
    }
}

// Fast vertical line
void GFXcanvas::drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t color) {
    if (!m_canvasBuffer || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > m_height) h = m_height - y;
    if (x < 0 || x >= m_width || h <= 0) return;

    uint32_t sa = color >> 24;
    if (sa == 0) return;

    for (int16_t row = y; row < y + h; row++)
        setPixelRaw(x, row, color);
}

// Override the internal fast-line hooks so circle / round-rect helpers work
void GFXcanvas::drawFastVLineInternal(int16_t x, int16_t y, int16_t h, uint32_t c) {
    drawFastVLine(x, y, h, c);
}
void GFXcanvas::drawFastHLineInternal(int16_t x, int16_t y, int16_t w, uint32_t c) {
    drawFastHLine(x, y, w, c);
}

// GFXcanvas::setPixel — override the LinuxGFX protected hook so
// writePixel() and all inherited primitives that call setPixel directly
// will write into the canvas buffer instead of the hardware buffer.
// getPixel() is handled by the public override which also applies rotation.

void GFXcanvas::setPixel(int16_t x, int16_t y, uint32_t color) {
    // Bounds already checked by caller (writePixel), but guard anyway.
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
    setPixelRaw(x, y, color);
}