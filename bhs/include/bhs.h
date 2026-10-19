/* Blackhawk Streaming (BHS) - C interface of the node library. Draft; see Simtary/bhs/PROTOCOL.md.
 *
 * Plain C: no exceptions or runtime types cross it, so an engine built without them can link it.
 * The host (an engine, or a test program) owns the world and the GPU frames; BHS owns the encoder,
 * WebRTC, signalling and the viewer. Windows / Direct3D only for now.
 *
 *   bhs_node* n = bhs_node_start(&cfg, err, sizeof err);
 *   every rendered frame:  if (bhs_node_has_viewer(n)) bhs_node_submit_shared(n, handle, w, h);
 *   bhs_node_stop(n);
 *
 * Threads: bhs_node_submit_shared is called from ONE host thread (the render thread). The input
 * and viewer callbacks arrive on BHS's own threads - the host must queue them.
 */
#ifndef BHS_H
#define BHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  ifdef BHS_BUILDING
#    define BHS_API __declspec(dllexport)
#  else
#    define BHS_API __declspec(dllimport)
#  endif
#else
#  define BHS_API
#endif

#define BHS_PROTOCOL_VERSION 1

/* Input event kinds = PROTOCOL.md 2.1. Key codes are USB HID usage ids (= SDL scancodes), see
 * keycodes.md. Fields by kind:
 *   1/2 key down/up        a = HID code
 *   3   mouse relative     a = dx, b = dy
 *   4   mouse absolute     a = x, b = y   (0..65535 of the video size)
 *   5   mouse button       a = button (0 left, 1 middle, 2 right), b = 1 down / 0 up
 *   6   wheel              a = dx, b = dy
 *   7   gamepad            a = pad, b = button bits (axes are parsed and dropped for now)
 *   8   touch              a = id, b = phase, c = (x << 16) | y
 */
typedef struct bhs_input_event {
    uint32_t viewer;
    uint32_t kind;
    int32_t a, b, c;
} bhs_input_event;

typedef void (*bhs_input_fn)(void* user, const bhs_input_event* event);
typedef void (*bhs_viewer_fn)(void* user, uint32_t viewer, int joined);

typedef struct bhs_config {
    uint32_t struct_size;        /* sizeof(bhs_config), for forward compatibility */
    uint16_t port;               /* signalling WebSocket, default 9000 */
    const char* bind_address;    /* default "127.0.0.1"; "0.0.0.0" to accept other machines */
    uint32_t width, height;      /* encoded size; frames of another size are scaled */
    uint32_t fps;                /* encode at most this many frames per second */
    uint32_t kbps;               /* video bitrate */
    void* user;
    bhs_input_fn on_input;       /* may be NULL */
    bhs_viewer_fn on_viewer;     /* may be NULL */
    int verbose;                 /* 1 = libdatachannel debug log */
} bhs_config;

typedef struct bhs_node bhs_node;

/* Starts the encoder, the signalling server and (on demand) the viewer's peer connection.
 * Returns NULL and fills `error` when there is no hardware GPU / encoder (no_gpu) or the port is taken. */
BHS_API bhs_node* bhs_node_start(const bhs_config* config, char* error, size_t error_size);
BHS_API void bhs_node_stop(bhs_node* node);

/* 1 while a viewer is connected and its video track is open: the host renders for it. */
BHS_API int bhs_node_has_viewer(const bhs_node* node);

/* Offers one finished frame: an NT shared handle of a 2D texture (D3D12 resource created with
 * a shared heap, or a D3D11 texture with MISC_SHARED_NTHANDLE). The GPU work that wrote it MUST be
 * complete. Returns 1 if the frame was encoded and sent, 0 if it was skipped (no viewer, or
 * faster than the configured fps), -1 on error. */
BHS_API int bhs_node_submit_shared(bhs_node* node, void* nt_handle, uint32_t width, uint32_t height);

BHS_API const char* bhs_version(void);

#ifdef __cplusplus
}
#endif
#endif
