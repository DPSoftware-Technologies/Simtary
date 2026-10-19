# BHS protocol - draft v1

Status: **draft, nothing implemented** (2026-10-19). Licence of this document and its reference
implementation: Apache-2.0. Design background: `../CLOUD.md`.

BHS is a native protocol. It reuses WebRTC for media and defines its own control layer. It does
not speak Epic's Pixel Streaming signalling. Reason: Pixel Streaming's messages are shaped around
one Unreal app per signalling server, while BHS needs many sessions per node and provider hooks.

Words: **gateway** (public entry: auth, signalling, placement), **node** (machine with a GPU,
runs the node agent), **session** (one engine process or one world, streamed), **viewer** (one
connected client of a session).

Rules for all layers:
- Versioned by the major number only: `bhs.v1`. A breaking change makes `bhs.v2`.
- Minor changes only ADD: new optional fields, new message types. Receivers ignore unknown fields
  and unknown message types (answering `unsupported` only when a reply is required).
- All text is UTF-8 JSON. All binary is little endian.

## 1. Signalling (viewer <-> gateway)

WebSocket, subprotocol `bhs.v1`, TLS required in production. Every message is a JSON object
`{"t": "<type>", "id": <u32, optional>, ...}`. A reply carries `"re": <id>`.

| type | from | fields | meaning |
|---|---|---|---|
| `hello` | viewer | `protocol:1`, `token`, `app`, `scenario`, `params{}`, `caps{video:["h264"],audio:["opus"],voice:bool}` | start; `token` is the provider's signed token (section 4) |
| `welcome` | gateway | `viewer`, `session`, `ice_servers[]`, `state` (`starting` / `ready`), `limits{}` | accepted; `ice_servers` carries TURN credentials |
| `state` | gateway | `session`, `state`, `progress` (0..1) | session starting or loading |
| `offer` / `answer` | either | `sdp` | normal WebRTC negotiation |
| `candidate` | either | `candidate` | trickle ICE |
| `attach` | viewer | `session` | join an existing session as another viewer (multi-view) |
| `bye` | either | `reason` | end the viewer connection |
| `error` | either | `code`, `message` | see error codes |

Order: `hello` -> `welcome` -> (`state` while the session starts) -> `offer` from the gateway side
(the node is the offerer) -> `answer` -> `candidate`s both ways. The viewer gets video as soon as
media flows; the first video frames may be the loading screen while the scene loads.

## 2. Data channels (viewer <-> session, over WebRTC)

| label | reliability | content |
|---|---|---|
| `input` | unordered, `maxRetransmits: 0` | binary input events (below) |
| `control` | ordered, reliable | JSON, same envelope as signalling |
| `app` | ordered, reliable | opaque bytes for the application (BHS never reads them) |

### 2.1 `input` frames

Each message is a sequence of events: `u16 seq` then events back to back. Event = `u8 kind` +
payload:

| kind | name | payload |
|---|---|---|
| 1 | key down | `u16 code` (USB HID usage id of the physical key, see `keycodes.md`), `u8 mods` |
| 2 | key up | same |
| 3 | mouse move relative | `i16 dx, i16 dy` |
| 4 | mouse move absolute | `u16 x, u16 y` (0..65535 of the video size) |
| 5 | mouse button | `u8 button, u8 down` |
| 6 | wheel | `i16 dx, i16 dy` |
| 7 | gamepad | `u8 pad, u16 buttons, i16 axes[6]` (state, not delta) |
| 8 | touch | `u8 id, u8 phase, u16 x, u16 y` |

The receiver drops frames whose `seq` is older than the newest seen. Gamepad state is a snapshot so
a lost frame costs nothing.

### 2.2 `control` messages

| type | from | fields |
|---|---|---|
| `quality` | viewer | `width, height, fps, max_bitrate` (a request; the node may grant less) |
| `stats` | node | `rtt_ms, bitrate, fps, encode_ms, dropped` (about 1 Hz) |
| `session_event` | node | `kind` (`loading`, `ready`, `ending`), `detail` |
| `limit` | node | `seconds_left` (session length limit about to end) |
| `clipboard` | either | `text` (off unless the provider allows it) |

## 3. Media

- Video: H.264 (constrained high, hardware encoded, low-latency, no B-frames, periodic intra
  refresh instead of big keyframes). AV1 and HEVC are optional extensions negotiated by `caps`.
- Audio session -> viewer: Opus, 48 kHz stereo.
- Voice viewer -> session (optional): the viewer's `offer` declares the audio line send+receive;
  otherwise receive-only. Allowed only if `limits.voice` is true.
- Resolution and frame rate follow `quality` requests; the node picks the encoder settings.

## 4. Provider hooks (gateway <-> provider)

BHS holds no accounts and no money. The provider supplies three HTTPS endpoints (or a plug-in in
the reference gateway). Requests are JSON and signed (`X-BHS-Signature`, HMAC-SHA256 with a shared
secret, over the body).

| hook | request | response |
|---|---|---|
| `authenticate` | `{token}` | `{ok, user}` where `user` is an opaque id, or `{ok:false}` |
| `authorize` | `{user, app, scenario, params}` | `{allow, limits{seconds, width, height, fps, voice, clipboard}, reason}` |
| `on_session_event` | `{session, user, event, time, usage{gpu_seconds, bytes}}` | `{}` (for metering and billing) |

`event`: `session_started`, `viewer_attached`, `viewer_left`, `session_ended`. If a hook times out
(default 3 s) `authenticate` and `authorize` DENY. `on_session_event` is retried and never blocks
the session.

The token format is the provider's business; the gateway only needs a way to verify it (JWT with
the provider's public key, or the `authenticate` hook).

## 5. Node API (gateway <-> node agent)

REST + JSON over HTTPS, bearer token between gateway and node. Prefix `/v1`.

| call | meaning |
|---|---|
| `GET /v1/health` | `{ok, gpu:{name, vram_total, vram_free}, encoder:{h264:true,...}, version}` |
| `GET /v1/capacity` | measured sessions that fit per quality level (see `../CLOUD.md`, Capacity) |
| `POST /v1/sessions` | body `{app, scenario, params, limits}`; reply `{session, state}` or an error |
| `GET /v1/sessions` / `GET /v1/sessions/{id}` | list / detail (state, viewers, uptime, usage) |
| `DELETE /v1/sessions/{id}` | end it |
| `GET /v1/metrics` | Prometheus text |

A node with no usable GPU or no hardware encoder reports `capacity: 0` and answers
`POST /v1/sessions` with `no_gpu`. It never falls back to software rendering.

## 6. Error codes

`bad_request`, `protocol` (version not supported), `auth_failed`, `forbidden` (authorize denied),
`app_unknown`, `capacity_full`, `no_gpu`, `session_not_found`, `session_failed` (engine crashed or
did not start), `timeout`, `unsupported`, `internal`.

Every error: `{"t":"error","code":"...","message":"human readable"}`. The viewer may retry
`capacity_full` and `timeout` only.

## 7. Host interface (the standalone C boundary)

What the engine side implements; no engine headers on the BHS side. Names are provisional.

```c
typedef struct bhs_host_desc {
    void  (*on_input)(void* user, uint32_t viewer, const bhs_input_event* e);
    void  (*on_audio_in)(void* user, uint32_t viewer, const int16_t* pcm, uint32_t frames);
    void  (*on_viewer)(void* user, uint32_t viewer, int joined);
    void*  user;
} bhs_host_desc;

bhs_session* bhs_session_create(const bhs_host_desc*);
// per frame, per viewer: a GPU texture handle + size + timestamp
void bhs_submit_video(bhs_session*, uint32_t viewer, bhs_texture, uint32_t w, uint32_t h, uint64_t pts_us);
void bhs_submit_audio(bhs_session*, const int16_t* pcm, uint32_t frames, uint64_t pts_us);
void bhs_session_destroy(bhs_session*);
```

Plain C, no exceptions or runtime types across it, so an engine built without RTTI can link it.

## 8. Not decided

- Exact WebRTC library (libdatachannel is the first candidate; it must expose what hardware
  encoded H.264 needs: pre-encoded frames in, RTP out).
- Whether the gateway is part of the reference node agent or a separate program.
- Clipboard and file transfer (only if a provider asks).
- AV1 / HEVC negotiation details.
- Reconnect: whether a viewer may resume a session after a dropped connection (likely yes, within
  a grace period, same `session` and a one-time resume token).
