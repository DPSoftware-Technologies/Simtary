// BHS test host: draws a test pattern on the GPU and streams it through the bhs library, the same
// way an engine does (a shared GPU texture per frame). Open index.html in a browser and connect;
// the mouse moves a crosshair and W/A/S/D a box in the picture, so the input round trip is visible.
//
//   bhs_stream_test [--port=9000] [--size=1280x720] [--fps=60] [--kbps=6000] [--log=debug]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "bhs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

const char* kShader = R"(
cbuffer C : register(b0) { float2 size; float time; float pad; float2 mouse; float2 box; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS (uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PS (VSOut i) : SV_Target {
    float2 p = i.uv * size;
    float3 bars = 0.5 + 0.5 * cos(6.2831 * (i.uv.x * 3.0 + time * 0.1 + float3(0, 0.33, 0.67)));
    float sweep = step(abs(p.x - fmod(time * 240.0, size.x)), 3.0);
    float grid = step(frac(p.x / 64.0), 0.03) + step(frac(p.y / 64.0), 0.03);
    float3 c = bars * (0.5 + 0.4 * i.uv.y) + sweep + grid * 0.4;
    float2 m = mouse * size;
    float cross = max(step(abs(p.x - m.x), 1.5) * step(abs(p.y - m.y), 24.0),
                      step(abs(p.y - m.y), 1.5) * step(abs(p.x - m.x), 24.0));
    c = lerp(c, float3(1, 1, 1), cross);
    float2 b = box * size;
    float inBox = step(abs(p.x - b.x), 30.0) * step(abs(p.y - b.y), 30.0);
    c = lerp(c, float3(1, 0.1, 0.1), inBox);
    return float4(saturate(c), 1);
}
)";

std::atomic<float> g_mouseX{0.5f}, g_mouseY{0.5f};
std::atomic<unsigned> g_keys{0};   // bit: 0 W, 1 A, 2 S, 3 D
std::atomic<unsigned> g_events{0};

void OnInput (void*, const bhs_input_event* e) {
    ++g_events;
    if (e->kind == 4) {
        g_mouseX = float(e->a) / 65535.0f;
        g_mouseY = float(e->b) / 65535.0f;
    } else if (e->kind == 1 || e->kind == 2) {
        // keycodes are USB HID usages: W = 26, A = 4, S = 22, D = 7
        unsigned bit = 0;
        if (e->a == 26) bit = 1; else if (e->a == 4) bit = 2; else if (e->a == 22) bit = 4; else if (e->a == 7) bit = 8;
        if (e->kind == 1) g_keys |= bit; else g_keys &= ~bit;
    }
}

void OnViewer (void*, uint32_t viewer, int joined) {
    printf("host: viewer %u %s\n", viewer, joined ? "joined" : "left");
    fflush(stdout);
}

} // namespace

int main (int argc, char** argv) {
    bhs_config cfg = {};
    cfg.struct_size = sizeof cfg;
    cfg.port = 9000;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fps = 60;
    cfg.kbps = 6000;
    cfg.on_input = OnInput;
    cfg.on_viewer = OnViewer;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--port=", 0) == 0) cfg.port = uint16_t(atoi(a.c_str() + 7));
        else if (a.rfind("--fps=", 0) == 0) cfg.fps = uint32_t(atoi(a.c_str() + 6));
        else if (a.rfind("--kbps=", 0) == 0) cfg.kbps = uint32_t(atoi(a.c_str() + 7));
        else if (a == "--log=debug") cfg.verbose = 1;
        else if (a.rfind("--size=", 0) == 0) {
            unsigned w = 0, h = 0;
            if (sscanf_s(a.c_str() + 7, "%ux%u", &w, &h) == 2) { cfg.width = w; cfg.height = h; }
        } else {
            printf("usage: bhs_stream_test [--port=9000] [--size=1280x720] [--fps=60] [--kbps=6000] [--log=debug]\n");
            return 2;
        }
    }

    char err[256] = {};
    bhs_node* node = bhs_node_start(&cfg, err, sizeof err);
    if (node == nullptr) {
        printf("bhs_node_start failed: %s\n", err);
        return 1;
    }

    // The host's own GPU device and a ring of shared frames (what the engine's DX12 side does).
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
            D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        printf("no D3D11 device\n");
        return 1;
    }
    ComPtr<ID3DBlob> vsb, psb, perr;
    D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &perr);
    D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &perr);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 32;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    device->CreateBuffer(&bd, nullptr, &cb);

    constexpr int kRing = 3;
    ComPtr<ID3D11Texture2D> tex[kRing];
    ComPtr<ID3D11RenderTargetView> rtv[kRing];
    HANDLE handle[kRing] = {};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = cfg.width;
    td.Height = cfg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (int i = 0; i < kRing; ++i) {
        device->CreateTexture2D(&td, nullptr, &tex[i]);
        device->CreateRenderTargetView(tex[i].Get(), nullptr, &rtv[i]);
        ComPtr<IDXGIResource1> res;
        tex[i].As(&res);
        res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle[i]);
    }
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> done;
    device->CreateQuery(&qd, &done);

    printf("open index.html in a browser (served over http, e.g. python -m http.server) and press connect. Ctrl+C to stop.\n");
    fflush(stdout);
    const auto start = Clock::now();
    auto next = start;
    uint64_t frame = 0;
    float bx = 0.5f, by = 0.5f;
    for (;;) {
        next += std::chrono::microseconds(1000000 / cfg.fps);
        std::this_thread::sleep_until(next);
        if (!bhs_node_has_viewer(node)) continue;   // nobody watching: no render

        const unsigned keys = g_keys.load();
        const float step = 0.6f / float(cfg.fps);
        if (keys & 1) by -= step;
        if (keys & 4) by += step;
        if (keys & 2) bx -= step;
        if (keys & 8) bx += step;
        bx = std::clamp(bx, 0.0f, 1.0f);
        by = std::clamp(by, 0.0f, 1.0f);
        const double t = std::chrono::duration<double>(Clock::now() - start).count();
        const float data[8] = { float(cfg.width), float(cfg.height), float(t), 0, g_mouseX.load(), g_mouseY.load(), bx, by };
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, data, 0, 0);
        const int slot = int(frame % kRing);
        ID3D11RenderTargetView* rt = rtv[slot].Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, float(cfg.width), float(cfg.height), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* b = cb.Get();
        ctx->PSSetConstantBuffers(0, 1, &b);
        ctx->Draw(3, 0);
        // The GPU work must be finished before another device reads the texture.
        ctx->End(done.Get());
        ctx->Flush();
        BOOL finished = FALSE;
        while (ctx->GetData(done.Get(), &finished, sizeof finished, 0) != S_OK || !finished) Sleep(0);

        bhs_node_submit_shared(node, handle[slot], cfg.width, cfg.height);
        ++frame;
    }
}
