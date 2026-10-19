// BHS milestone 1: test pattern -> GPU texture -> hardware H.264 -> MP4. No engine.
//
//   bhs_encode_test [out.mp4] [--seconds=10] [--size=1920x1080] [--fps=60] [--kbps=8000]
//                   [--sessions=1]   (N encoders at once, to find the concurrent-encode limit)
//
// Each frame is drawn on the GPU (a shader fills a BGRA texture), handed to the Media Foundation
// sink writer as a D3D11 texture (no CPU copy) and encoded by whatever hardware H.264 encoder the
// machine has (NVENC / AMF / Quick Sync behind Media Foundation). It prints which encoder it got
// and the time per frame, which is the number the BHS capacity plan needs.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::wstring out = L"bhs_test.mp4";
    int seconds = 10;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int kbps = 8000;
    int sessions = 1;
};

bool Check (HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("FAILED: %s (0x%08X)\n", what, unsigned(hr));
        return false;
    }
    return true;
}

const char* kShader = R"(
cbuffer C : register(b0) { float2 size; float time; float pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS (uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PS (VSOut i) : SV_Target {
    float2 p = i.uv * size;
    // moving colour bars, a sweeping white line and a grid: enough detail to see encode quality
    float3 bars = 0.5 + 0.5 * cos(6.2831 * (i.uv.x * 3.0 + time * 0.1 + float3(0, 0.33, 0.67)));
    float line_ = step(abs(p.x - fmod(time * 240.0, size.x)), 3.0);
    float grid = step(frac(p.x / 64.0), 0.03) + step(frac(p.y / 64.0), 0.03);
    float3 c = bars * (0.6 + 0.4 * i.uv.y) + line_ + grid * 0.5;
    return float4(saturate(c), 1);
}
)";

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    std::wstring adapterName;
};

bool CreateGpu (Gpu& g) {
    ComPtr<IDXGIFactory1> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
    ComPtr<IDXGIAdapter1> adapter;
    // The first adapter that is a real GPU (skip the software "Basic Render Driver").
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            g.adapterName = d.Description;
            break;
        }
        adapter.Reset();
    }
    if (!adapter) {
        printf("no hardware GPU found - BHS needs one (error code: no_gpu)\n");
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
            D3D11_SDK_VERSION, &g.device, nullptr, &g.context), "D3D11CreateDevice")) return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g.device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    UINT token = 0;
    if (!Check(MFCreateDXGIDeviceManager(&token, &g.manager), "MFCreateDXGIDeviceManager")) return false;
    if (!Check(g.manager->ResetDevice(g.device.Get(), token), "ResetDevice")) return false;

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err))) {
        printf("shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    g.device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    g.device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.device->CreateBuffer(&bd, nullptr, &g.cb);
    return true;
}

void PrintEncoders () {
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, &out, &acts, &n))) {
        printf("hardware H.264 encoders: %u\n", n);
        for (UINT32 i = 0; i < n; ++i) {
            WCHAR* name = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
                printf("  %ls\n", name);
                CoTaskMemFree(name);
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
}

struct Result {
    bool ok = false;
    int frames = 0;
    double totalMs = 0, maxMs = 0, avgMs = 0;
};

// One encoding session: its own sink writer + render target ring, sharing the GPU.
Result RunSession (Gpu& g, const Options& o, const std::wstring& path, int index) {
    Result r;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 4);
    attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, g.manager.Get());
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFSinkWriter> writer;
    if (!Check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer), "MFCreateSinkWriterFromURL")) return r;

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(o.kbps) * 1000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, o.width, o.height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, o.fps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    if (!Check(writer->AddStream(outType.Get(), &stream), "AddStream")) return r;

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, o.width, o.height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, o.fps, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (!Check(writer->SetInputMediaType(stream, inType.Get(), nullptr), "SetInputMediaType")) return r;
    if (!Check(writer->BeginWriting(), "BeginWriting")) return r;

    // A small ring: the encoder holds a texture until it is done with it.
    constexpr int kRing = 8;
    std::vector<ComPtr<ID3D11Texture2D>> tex(kRing);
    std::vector<ComPtr<ID3D11RenderTargetView>> rtv(kRing);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = o.width;
    td.Height = o.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < kRing; ++i) {
        if (!Check(g.device->CreateTexture2D(&td, nullptr, &tex[i]), "CreateTexture2D")) return r;
        g.device->CreateRenderTargetView(tex[i].Get(), nullptr, &rtv[i]);
    }

    const int total = o.seconds * o.fps;
    const LONGLONG frameDur = 10000000LL / o.fps;   // 100 ns units
    using Clock = std::chrono::steady_clock;
    for (int f = 0; f < total; ++f) {
        const int slot = f % kRing;
        // draw
        float cbData[4] = { float(o.width), float(o.height), float(f) / float(o.fps) + float(index), 0 };
        g.context->UpdateSubresource(g.cb.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11RenderTargetView* rt = rtv[slot].Get();
        g.context->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, float(o.width), float(o.height), 0, 1 };
        g.context->RSSetViewports(1, &vp);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(g.vs.Get(), nullptr, 0);
        g.context->PSSetShader(g.ps.Get(), nullptr, 0);
        ID3D11Buffer* cb = g.cb.Get();
        g.context->PSSetConstantBuffers(0, 1, &cb);
        g.context->Draw(3, 0);

        // hand the texture to the encoder as is
        ComPtr<IMFMediaBuffer> buffer;
        if (!Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex[slot].Get(), 0, FALSE, &buffer), "MFCreateDXGISurfaceBuffer")) return r;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2d;
        if (SUCCEEDED(buffer.As(&b2d))) b2d->GetContiguousLength(&len);
        buffer->SetCurrentLength(len);
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(LONGLONG(f) * frameDur);
        sample->SetSampleDuration(frameDur);

        const auto t0 = Clock::now();
        if (!Check(writer->WriteSample(stream, sample.Get()), "WriteSample")) return r;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        r.totalMs += ms;
        r.maxMs = std::max(r.maxMs, ms);
        ++r.frames;
    }
    if (!Check(writer->Finalize(), "Finalize")) return r;
    r.avgMs = r.frames > 0 ? r.totalMs / r.frames : 0;
    r.ok = true;
    return r;
}

bool ParseArgs (int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto val = [&](const wchar_t* key, int& dst) {
            const std::wstring k = key;
            if (a.rfind(k, 0) == 0) { dst = _wtoi(a.c_str() + k.size()); return true; }
            return false;
        };
        if (val(L"--seconds=", o.seconds) || val(L"--fps=", o.fps) || val(L"--kbps=", o.kbps) || val(L"--sessions=", o.sessions)) continue;
        if (a.rfind(L"--size=", 0) == 0) {
            if (swscanf_s(a.c_str() + 7, L"%dx%d", &o.width, &o.height) != 2) return false;
            continue;
        }
        if (a.rfind(L"--", 0) == 0) return false;
        o.out = a;
    }
    // H.264 wants even sizes.
    o.width &= ~1;
    o.height &= ~1;
    return o.width > 0 && o.height > 0 && o.fps > 0 && o.seconds > 0 && o.sessions > 0;
}

} // namespace

int wmain (int argc, wchar_t** argv) {
    Options o;
    if (!ParseArgs(argc, argv, o)) {
        printf("usage: bhs_encode_test [out.mp4] [--seconds=10] [--size=1920x1080] [--fps=60] [--kbps=8000] [--sessions=1]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!Check(MFStartup(MF_VERSION), "MFStartup")) return 1;

    Gpu gpu;
    if (!CreateGpu(gpu)) return 1;
    printf("GPU: %ls\n", gpu.adapterName.c_str());
    PrintEncoders();
    printf("%d session(s), %dx%d @ %d fps, %d kbps, %d s each\n", o.sessions, o.width, o.height, o.fps, o.kbps, o.seconds);

    const auto start = std::chrono::steady_clock::now();
    std::vector<Result> results(o.sessions);
    if (o.sessions == 1) {
        results[0] = RunSession(gpu, o, o.out, 0);
    } else {
        std::vector<std::thread> threads;
        for (int i = 0; i < o.sessions; ++i) {
            std::wstring path = o.out;
            const size_t dot = path.rfind(L'.');
            path.insert(dot == std::wstring::npos ? path.size() : dot, L"_" + std::to_wstring(i));
            threads.emplace_back([&, i, path] { results[i] = RunSession(gpu, o, path, i); });
        }
        for (std::thread& t : threads) t.join();
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    int failed = 0;
    for (int i = 0; i < o.sessions; ++i) {
        const Result& r = results[i];
        if (!r.ok) { ++failed; printf("session %d: FAILED after %d frames\n", i, r.frames); continue; }
        printf("session %d: %d frames, WriteSample avg %.2f ms, max %.2f ms\n", i, r.frames, r.avgMs, r.maxMs);
    }
    const int frames = o.seconds * o.fps * o.sessions;
    printf("wall %.2f s for %d frames = %.1f fps total (%.1fx realtime per session)\n",
           wall, frames, frames / wall, (frames / wall) / (o.fps * o.sessions));
    MFShutdown();
    return failed == 0 ? 0 : 1;
}
