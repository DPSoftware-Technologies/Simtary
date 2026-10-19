// Which D3D12 resources can a D3D11 device open through a shared NT handle? Prints a matrix.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

int main () {
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { printf("adapter: %ls\n", d.Description); break; }
        adapter.Reset();
    }
    ComPtr<ID3D12Device> dev12;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev12)))) { printf("no d3d12\n"); return 1; }
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev11, nullptr, &ctx);
    ComPtr<ID3D11Device1> dev11_1;
    dev11.As(&dev11_1);

    struct F { const char* name; D3D12_RESOURCE_FLAGS flags; } flags[] = {
        { "NONE", D3D12_RESOURCE_FLAG_NONE },
        { "SIMULTANEOUS", D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS },
        { "RT", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
        { "RT|SIMULTANEOUS", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS },
    };
    struct Fmt { const char* name; DXGI_FORMAT f; } fmts[] = {
        { "B8G8R8A8", DXGI_FORMAT_B8G8R8A8_UNORM }, { "R8G8B8A8", DXGI_FORMAT_R8G8B8A8_UNORM },
        { "R10G10B10A2", DXGI_FORMAT_R10G10B10A2_UNORM }, { "R8G8B8A8_TYPELESS", DXGI_FORMAT_R8G8B8A8_TYPELESS },
    };
    for (const Fmt& fm : fmts) {
        for (const F& fl : flags) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 1280;
            rd.Height = 720;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = fm.f;
            rd.SampleDesc.Count = 1;
            rd.Flags = fl.flags;
            ComPtr<ID3D12Resource> res;
            HRESULT hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
            if (FAILED(hr)) { printf("%-18s %-16s create 0x%08X\n", fm.name, fl.name, unsigned(hr)); continue; }
            HANDLE h = nullptr;
            hr = dev12->CreateSharedHandle(res.Get(), nullptr, GENERIC_ALL, nullptr, &h);
            if (FAILED(hr)) { printf("%-18s %-16s handle 0x%08X\n", fm.name, fl.name, unsigned(hr)); continue; }
            ComPtr<ID3D11Texture2D> t11;
            hr = dev11_1->OpenSharedResource1(h, IID_PPV_ARGS(&t11));
            printf("%-18s %-16s open 0x%08X %s\n", fm.name, fl.name, unsigned(hr), SUCCEEDED(hr) ? "ok" : "FAIL");
            CloseHandle(h);
        }
    }
    return 0;
}
