// BHS node library: shared GPU frame -> hardware H.264 -> WebRTC -> one browser viewer.
// C interface in ../include/bhs.h, wire protocol in ../PROTOCOL.md. Windows / Direct3D 11 + Media
// Foundation. This file may use exceptions and RTTI (libdatachannel does); none of it crosses the
// C boundary.

#define BHS_BUILDING 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <wrl/client.h>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "bhs.h"

#include <algorithm>
#include <cstdarg>
#include <variant>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

void Say (const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("BHS: ");
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

bool Check (HRESULT hr, const char* what, std::string& error) {
    if (FAILED(hr)) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s failed (0x%08X)", what, unsigned(hr));
        error = buf;
        return false;
    }
    return true;
}

// ---- GPU ------------------------------------------------------------------------------------

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    LUID luid = {};
    std::wstring adapterName;

    bool Init (std::string& error) {
        ComPtr<IDXGIFactory1> factory;
        if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1", error)) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            adapter->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapterName = d.Description;
                luid = d.AdapterLuid;
                break;
            }
            adapter.Reset();
        }
        if (!adapter) {
            error = "no_gpu: no hardware GPU found";
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        if (!Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice", error)) return false;
        device.As(&device1);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        if (!Check(MFCreateDXGIDeviceManager(&token, &manager), "MFCreateDXGIDeviceManager", error)) return false;
        return Check(manager->ResetDevice(device.Get(), token), "ResetDevice", error);
    }
};

// ---- colour conversion on the GPU -------------------------------------------------------------

struct Converter {
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> en;
    ComPtr<ID3D11VideoProcessor> vp;
    UINT inW = 0, inH = 0, outW = 0, outH = 0;

    bool Init (Gpu& g, UINT iw, UINT ih, UINT ow, UINT oh, std::string& error) {
        inW = iw; inH = ih; outW = ow; outH = oh;
        en.Reset(); vp.Reset();
        if (!Check(g.device.As(&vdev), "ID3D11VideoDevice", error) || !Check(g.context.As(&vctx), "ID3D11VideoContext", error)) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = iw;
        cd.InputHeight = ih;
        cd.OutputWidth = ow;
        cd.OutputHeight = oh;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (!Check(vdev->CreateVideoProcessorEnumerator(&cd, &en), "CreateVideoProcessorEnumerator", error)) return false;
        if (!Check(vdev->CreateVideoProcessor(en.Get(), 0, &vp), "CreateVideoProcessor", error)) return false;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
        in.RGB_Range = 0;
        in.YCbCr_Matrix = 1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
        return true;
    }

    bool Convert (ID3D11Texture2D* src, ID3D11Texture2D* dst, std::string& error) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        if (!Check(vdev->CreateVideoProcessorInputView(src, en.Get(), &ivd, &iv), "CreateVideoProcessorInputView", error)) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        if (!Check(vdev->CreateVideoProcessorOutputView(dst, en.Get(), &ovd, &ov), "CreateVideoProcessorOutputView", error)) return false;
        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = iv.Get();
        return Check(vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st), "VideoProcessorBlt", error);
    }
};

// ---- hardware H.264 encoder (Media Foundation transform, asynchronous) ---------------------------

struct Packet {
    std::vector<uint8_t> data;
    bool key = false;
    double pts = 0;
};

bool HasNal (const std::vector<uint8_t>& d, int type) {
    for (size_t i = 0; i + 4 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            const size_t n = d[i + 2] == 1 ? i + 3 : i + 4;
            if (n < d.size() && (d[n] & 0x1f) == type) return true;
        }
    }
    return false;
}

struct Encoder {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> gen;
    ComPtr<ICodecAPI> codec;
    std::deque<Packet> out;
    std::vector<uint8_t> header;   // SPS/PPS from the output type, used if keyframes lack them
    std::wstring name;
    int needInput = 0;
    int fps = 60;

    void SetCodec (const GUID& key, ULONG value) {
        if (!codec) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = value;
        codec->SetValue(&key, &v);
    }

    bool Init (Gpu& g, UINT w, UINT h, int fpsIn, int kbps, std::string& error) {
        fps = fpsIn;
        MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
        ComPtr<IMFAttributes> filter;
        MFCreateAttributes(&filter, 1);
        filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&g.luid), sizeof(LUID));
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        if (FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                &inInfo, &outInfo, filter.Get(), &acts, &n)) || n == 0) {
            error = "no_gpu: no hardware H.264 encoder on this GPU";
            return false;
        }
        bool created = false;
        for (UINT32 i = 0; i < n; ++i) {
            if (!created) {
                WCHAR* fn = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) {
                    name = fn;
                    CoTaskMemFree(fn);
                }
                created = SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)));
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        if (!created) {
            error = "could not create the hardware encoder";
            return false;
        }
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (!Check(mft.As(&gen), "IMFMediaEventGenerator (not an async encoder?)", error)) return false;
        if (!Check(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(g.manager.Get())), "SET_D3D_MANAGER", error)) return false;

        ComPtr<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, UINT32(kbps) * 1000);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (!Check(mft->SetOutputType(0, ot.Get(), 0), "SetOutputType", error)) return false;

        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (!Check(mft->SetInputType(0, it.Get(), 0), "SetInputType", error)) return false;

        mft.As(&codec);
        if (codec) {
            VARIANT t;
            VariantInit(&t);
            t.vt = VT_BOOL;
            t.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &t);
        }
        SetCodec(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodec(CODECAPI_AVEncCommonMeanBitRate, UINT32(kbps) * 1000);
        SetCodec(CODECAPI_AVEncMPVGOPSize, UINT32(fps) * 4);
        SetCodec(CODECAPI_AVEncMPVDefaultBPictureCount, 0);

        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, &cur))) {
            UINT32 size = 0;
            if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
                header.resize(size);
                cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr);
            }
        }
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void ForceKeyFrame () { SetCodec(CODECAPI_AVEncVideoForceKeyFrame, 1); }

    void ReadOutput () {
        MFT_OUTPUT_STREAM_INFO si = {};
        mft->GetOutputStreamInfo(0, &si);
        const bool provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ComPtr<IMFSample> own;
        if (!provides) {
            MFCreateSample(&own);
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(si.cbSize > 0 ? si.cbSize : 1 << 20, &buf);
            own->AddBuffer(buf.Get());
            ob.pSample = own.Get();
            own->AddRef();
        }
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
        if (SUCCEEDED(hr) && ob.pSample != nullptr) {
            ComPtr<IMFMediaBuffer> buf;
            ob.pSample->ConvertToContiguousBuffer(&buf);
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                Packet pk;
                pk.data.assign(p, p + len);
                buf->Unlock();
                UINT32 clean = 0;
                ob.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
                pk.key = clean != 0 || HasNal(pk.data, 5);
                LONGLONG t = 0;
                ob.pSample->GetSampleTime(&t);
                pk.pts = double(t) / 1e7;
                if (pk.key && !header.empty() && !HasNal(pk.data, 7)) {
                    pk.data.insert(pk.data.begin(), header.begin(), header.end());
                }
                out.push_back(std::move(pk));
            }
        }
        if (ob.pSample) ob.pSample->Release();
        if (ob.pEvents) ob.pEvents->Release();
    }

    void Pump () {
        for (;;) {
            ComPtr<IMFMediaEvent> ev;
            if (gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev) != S_OK) return;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput) ++needInput;
            else if (type == METransformHaveOutput) ReadOutput();
        }
    }

    bool Submit (ID3D11Texture2D* nv12, LONGLONG time100ns, std::string& error) {
        for (int i = 0; i < 100 && needInput == 0; ++i) {
            Pump();
            if (needInput == 0) Sleep(1);
        }
        if (needInput == 0) {
            error = "encoder is behind";
            return false;
        }
        ComPtr<IMFMediaBuffer> buf;
        if (!Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buf), "MFCreateDXGISurfaceBuffer", error)) return false;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2d;
        if (SUCCEEDED(buf.As(&b2d))) b2d->GetContiguousLength(&len);
        buf->SetCurrentLength(len);
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time100ns);
        sample->SetSampleDuration(10000000LL / fps);
        if (!Check(mft->ProcessInput(0, sample.Get(), 0), "ProcessInput", error)) return false;
        --needInput;
        return true;
    }
};

// ---- viewer ---------------------------------------------------------------------------------

struct Viewer {
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> inputCh, controlCh;
    std::atomic<bool> wantKey{true};
    std::atomic<bool> closed{false};
    std::atomic<int> lastSeq{-1};
    uint32_t id = 0;
};

} // namespace

struct bhs_node {
    bhs_config cfg = {};
    std::string bindAddress;
    Gpu gpu;
    Converter conv;
    Encoder enc;
    constexpr static int kRing = 4;
    ComPtr<ID3D11Texture2D> nv12[kRing];
    std::unique_ptr<rtc::WebSocketServer> server;

    std::mutex mu;
    std::shared_ptr<Viewer> viewer;
    std::vector<std::shared_ptr<rtc::WebSocket>> sockets;   // the server hands over the only reference
    uint32_t nextViewer = 1;
    std::atomic<bool> hasViewer{false};

    std::map<void*, ComPtr<ID3D11Texture2D>> opened;        // shared handle -> D3D11 view of it
    uint64_t frame = 0;
    Clock::time_point lastEncode = Clock::now() - std::chrono::hours(1);
    Clock::time_point statsAt = Clock::now();
    int sentFrames = 0;
    int sentBytes = 0;
    double encodeMs = 0;

    void Send (const std::shared_ptr<rtc::WebSocket>& ws, const json& j) {
        if (ws && ws->isOpen()) ws->send(j.dump());
    }
    void SendError (const std::shared_ptr<rtc::WebSocket>& ws, const char* code, const char* msg) {
        Send(ws, { { "t", "error" }, { "code", code }, { "message", msg } });
    }
    void ParseInput (uint32_t viewerId, Viewer& v, const rtc::binary& m);
    void StartViewer (const std::shared_ptr<Viewer>& v);
    void OnSignal (const std::shared_ptr<rtc::WebSocket>& ws, const std::string& text);
    void Refresh ();
};

namespace {

// Frame = u16 seq, then events (kind u8 + payload); layout in PROTOCOL.md 2.1.
}

void bhs_node::ParseInput (uint32_t viewerId, Viewer& v, const rtc::binary& m) {
    if (m.size() < 2 || cfg.on_input == nullptr) return;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(m.data());
    const size_t n = m.size();
    const uint16_t seq = uint16_t(d[0] | (d[1] << 8));
    const int last = v.lastSeq.load();
    if (last >= 0 && int16_t(uint16_t(seq - uint16_t(last))) <= 0) return;   // older than the newest seen
    v.lastSeq = seq;
    size_t o = 2;
    auto u16 = [&](size_t at) { return int32_t(uint16_t(d[at] | (d[at + 1] << 8))); };
    auto i16 = [&](size_t at) { return int32_t(int16_t(d[at] | (d[at + 1] << 8))); };
    while (o < n) {
        bhs_input_event e = {};
        e.viewer = viewerId;
        e.kind = d[o++];
        size_t need = 0;
        switch (e.kind) {
            case 1: case 2: need = 3; break;
            case 3: case 4: case 6: need = 4; break;
            case 5: need = 2; break;
            case 7: need = 15; break;
            case 8: need = 6; break;
            default: return;   // unknown kind: the rest of the frame cannot be parsed
        }
        if (o + need > n) return;
        switch (e.kind) {
            case 1: case 2: e.a = u16(o); break;
            case 3: case 6: e.a = i16(o); e.b = i16(o + 2); break;
            case 4: e.a = u16(o); e.b = u16(o + 2); break;
            case 5: e.a = d[o]; e.b = d[o + 1]; break;
            case 7: e.a = d[o]; e.b = u16(o + 1); break;
            case 8: e.a = d[o]; e.b = d[o + 1]; e.c = (u16(o + 2) << 16) | u16(o + 4); break;
        }
        cfg.on_input(cfg.user, &e);
        o += need;
    }
}

void bhs_node::StartViewer (const std::shared_ptr<Viewer>& v) {
    rtc::Configuration rc;
    rc.disableAutoNegotiation = true;
    v->pc = std::make_shared<rtc::PeerConnection>(rc);
    std::weak_ptr<Viewer> wv = v;

    v->pc->onStateChange([wv](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Disconnected || s == rtc::PeerConnection::State::Failed ||
            s == rtc::PeerConnection::State::Closed) {
            if (auto v2 = wv.lock()) v2->closed = true;
        }
    });
    v->pc->onLocalDescription([this, wv](rtc::Description d) {
        if (auto v2 = wv.lock()) Send(v2->ws, { { "t", "offer" }, { "sdp", std::string(d) } });
    });
    v->pc->onLocalCandidate([this, wv](rtc::Candidate c) {
        if (auto v2 = wv.lock()) Send(v2->ws, { { "t", "candidate" }, { "candidate", c.candidate() }, { "mid", c.mid() } });
    });

    const uint32_t ssrc = 42;
    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e028");
    video.addSSRC(ssrc, "bhs");
    v->track = v->pc->addTrack(video);
    auto rtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "bhs", 96, rtc::H264RtpPacketizer::ClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpCfg);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpCfg));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([wv] {
        if (auto v2 = wv.lock()) v2->wantKey = true;
    }));
    v->track->setMediaHandler(packetizer);
    v->track->onOpen([wv] {
        if (auto v2 = wv.lock()) v2->wantKey = true;
    });

    rtc::DataChannelInit init;
    init.reliability.unordered = true;
    init.reliability.maxRetransmits = 0;
    v->inputCh = v->pc->createDataChannel("input", init);
    const uint32_t id = v->id;
    v->inputCh->onMessage([this, wv, id](std::variant<rtc::binary, rtc::string> m) {
        auto v2 = wv.lock();
        if (v2 && std::holds_alternative<rtc::binary>(m)) ParseInput(id, *v2, std::get<rtc::binary>(m));
    });
    v->controlCh = v->pc->createDataChannel("control");
    v->pc->setLocalDescription();
}

void bhs_node::OnSignal (const std::shared_ptr<rtc::WebSocket>& ws, const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (m.is_discarded() || !m.is_object() || !m.contains("t") || !m["t"].is_string()) {
        SendError(ws, "bad_request", "not a BHS message");
        return;
    }
    const std::string t = m["t"].get<std::string>();
    std::shared_ptr<Viewer> v;
    {
        std::scoped_lock lock(mu);
        if (viewer && viewer->ws == ws) v = viewer;
    }
    if (t == "hello") {
        if (m.value("protocol", 0) != BHS_PROTOCOL_VERSION) {
            SendError(ws, "protocol", "this node speaks bhs.v1");
            ws->close();
            return;
        }
        std::scoped_lock lock(mu);
        if (viewer && !viewer->closed) {
            SendError(ws, "capacity_full", "this node serves one viewer");
            ws->close();
            return;
        }
        auto nv = std::make_shared<Viewer>();
        nv->ws = ws;
        nv->id = nextViewer++;
        viewer = nv;
        Send(ws, { { "t", "welcome" }, { "viewer", nv->id }, { "session", "node-1" }, { "ice_servers", json::array() },
                   { "state", "ready" }, { "limits", json::object() } });
        Say("viewer %u connected (%s)", nv->id, m.value("scenario", "").c_str());
        StartViewer(nv);
        if (cfg.on_viewer) cfg.on_viewer(cfg.user, nv->id, 1);
    } else if (!v) {
        SendError(ws, "bad_request", "send hello first");
    } else if (t == "answer") {
        v->pc->setRemoteDescription(rtc::Description(m.value("sdp", ""), "answer"));
    } else if (t == "candidate") {
        v->pc->addRemoteCandidate(rtc::Candidate(m.value("candidate", ""), m.value("mid", "")));
    } else if (t == "bye") {
        v->closed = true;
    }
}

// Retire a viewer whose connection ended; keeps hasViewer in step.
void bhs_node::Refresh () {
    std::shared_ptr<Viewer> gone;
    {
        std::scoped_lock lock(mu);
        if (viewer && viewer->closed) {
            gone = viewer;
            viewer.reset();
        }
        hasViewer = viewer && viewer->track && viewer->track->isOpen();
    }
    if (gone) {
        Say("viewer %u gone", gone->id);
        if (gone->pc) gone->pc->close();
        if (cfg.on_viewer) cfg.on_viewer(cfg.user, gone->id, 0);
    }
}

extern "C" {

BHS_API const char* bhs_version (void) { return "0.1 draft (protocol 1)"; }

BHS_API bhs_node* bhs_node_start (const bhs_config* config, char* error, size_t errorSize) {
    std::string err;
    auto fail = [&](const std::string& e) -> bhs_node* {
        if (error != nullptr && errorSize > 0) snprintf(error, errorSize, "%s", e.c_str());
        return nullptr;
    };
    if (config == nullptr || config->struct_size < sizeof(bhs_config)) return fail("bad bhs_config");
    auto node = std::make_unique<bhs_node>();
    node->cfg = *config;
    if (node->cfg.port == 0) node->cfg.port = 9000;
    if (node->cfg.fps == 0) node->cfg.fps = 60;
    if (node->cfg.kbps == 0) node->cfg.kbps = 6000;
    node->cfg.width &= ~1u;
    node->cfg.height &= ~1u;
    if (node->cfg.width == 0 || node->cfg.height == 0) return fail("bad size");
    node->bindAddress = (config->bind_address != nullptr && config->bind_address[0] != 0) ? config->bind_address : "127.0.0.1";

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!Check(MFStartup(MF_VERSION), "MFStartup", err)) return fail(err);
    rtc::InitLogger(node->cfg.verbose ? rtc::LogLevel::Debug : rtc::LogLevel::Warning);

    if (!node->gpu.Init(err)) return fail(err);
    if (!node->enc.Init(node->gpu, node->cfg.width, node->cfg.height, int(node->cfg.fps), int(node->cfg.kbps), err)) return fail(err);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = node->cfg.width;
    td.Height = node->cfg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < bhs_node::kRing; ++i) {
        if (!Check(node->gpu.device->CreateTexture2D(&td, nullptr, &node->nv12[i]), "CreateTexture2D(NV12)", err)) return fail(err);
    }

    bhs_node* raw = node.get();
    rtc::WebSocketServer::Configuration wsc;
    wsc.port = node->cfg.port;
    wsc.bindAddress = node->bindAddress;
    try {
        node->server = std::make_unique<rtc::WebSocketServer>(wsc);
    } catch (const std::exception& e) {
        return fail(std::string("cannot listen on port ") + std::to_string(node->cfg.port) + ": " + e.what());
    }
    node->server->onClient([raw](std::shared_ptr<rtc::WebSocket> ws) {
        std::weak_ptr<rtc::WebSocket> w = ws;
        {
            std::scoped_lock lock(raw->mu);
            raw->sockets.push_back(ws);
        }
        ws->onMessage([raw, w](std::variant<rtc::binary, rtc::string> m) {
            auto s = w.lock();
            if (s && std::holds_alternative<rtc::string>(m)) raw->OnSignal(s, std::get<rtc::string>(m));
        });
        ws->onClosed([raw, w] {
            std::scoped_lock lock(raw->mu);
            if (auto s = w.lock()) {
                if (raw->viewer && raw->viewer->ws == s) raw->viewer->closed = true;
                raw->sockets.erase(std::remove(raw->sockets.begin(), raw->sockets.end(), s), raw->sockets.end());
            }
        });
    });
    Say("GPU %ls, encoder %ls, %ux%u @ %u fps, %u kbps; signalling ws://%s:%u",
        node->gpu.adapterName.c_str(), node->enc.name.c_str(), node->cfg.width, node->cfg.height,
        node->cfg.fps, node->cfg.kbps, node->bindAddress.c_str(), unsigned(node->cfg.port));
    return node.release();
}

BHS_API void bhs_node_stop (bhs_node* node) {
    if (node == nullptr) return;
    node->server.reset();
    {
        std::scoped_lock lock(node->mu);
        if (node->viewer && node->viewer->pc) node->viewer->pc->close();
        node->viewer.reset();
        node->sockets.clear();
    }
    delete node;
    MFShutdown();
}

BHS_API int bhs_node_has_viewer (const bhs_node* node) {
    if (node == nullptr) return 0;
    const_cast<bhs_node*>(node)->Refresh();
    return node->hasViewer ? 1 : 0;
}

BHS_API int bhs_node_submit_shared (bhs_node* node, void* handle, uint32_t width, uint32_t height) {
    if (node == nullptr || handle == nullptr) return -1;
    node->Refresh();
    if (!node->hasViewer) return 0;
    const auto now = Clock::now();
    const auto period = std::chrono::microseconds(1000000 / node->cfg.fps);
    if (now - node->lastEncode < period * 9 / 10) return 0;   // faster than the configured fps
    std::shared_ptr<Viewer> v;
    {
        std::scoped_lock lock(node->mu);
        v = node->viewer;
    }
    if (!v) return 0;

    std::string err;
    ComPtr<ID3D11Texture2D>& tex = node->opened[handle];
    if (!tex) {
        if (!node->gpu.device1 ||
            !Check(node->gpu.device1->OpenSharedResource1(HANDLE(handle), IID_PPV_ARGS(&tex)), "OpenSharedResource1", err)) {
            node->opened.erase(handle);
            Say("%s", err.c_str());
            return -1;
        }
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        Say("opened shared frame %ux%u, DXGI format %d", d.Width, d.Height, int(d.Format));
    }
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (node->conv.vp == nullptr || node->conv.inW != d.Width || node->conv.inH != d.Height) {
        if (!node->conv.Init(node->gpu, d.Width, d.Height, node->cfg.width, node->cfg.height, err)) {
            Say("%s", err.c_str());
            return -1;
        }
    }

    const auto t0 = Clock::now();
    ID3D11Texture2D* dst = node->nv12[node->frame % bhs_node::kRing].Get();
    if (v->wantKey.exchange(false)) node->enc.ForceKeyFrame();
    const LONGLONG pts = LONGLONG(node->frame) * (10000000LL / node->cfg.fps);
    if (!node->conv.Convert(tex.Get(), dst, err) || !node->enc.Submit(dst, pts, err)) {
        Say("%s", err.c_str());
        return -1;
    }
    ++node->frame;
    node->lastEncode = now;
    node->enc.Pump();
    node->encodeMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    while (!node->enc.out.empty()) {
        Packet& pk = node->enc.out.front();
        if (v->track && v->track->isOpen()) {
            v->track->sendFrame(reinterpret_cast<const std::byte*>(pk.data.data()), pk.data.size(),
                                rtc::FrameInfo(std::chrono::duration<double>(pk.pts)));
            ++node->sentFrames;
            node->sentBytes += int(pk.data.size());
        }
        node->enc.out.pop_front();
    }

    if (Clock::now() - node->statsAt >= std::chrono::seconds(1)) {
        const double secs = std::chrono::duration<double>(Clock::now() - node->statsAt).count();
        node->statsAt = Clock::now();
        const int kbps = int(node->sentBytes * 8 / secs / 1000);
        const double ms = node->sentFrames > 0 ? node->encodeMs / node->sentFrames : 0.0;
        if (v->controlCh && v->controlCh->isOpen()) {
            const json stats = { { "t", "stats" }, { "fps", int(node->sentFrames / secs + 0.5) }, { "encode_ms", ms }, { "bitrate", kbps } };
            v->controlCh->send(stats.dump());
        }
        Say("sent %d frames, %d kbps, convert+encode %.2f ms/frame", node->sentFrames, kbps, ms);
        node->sentFrames = node->sentBytes = 0;
        node->encodeMs = 0;
    }
    (void)width;
    (void)height;
    return 1;
}

} // extern "C"
