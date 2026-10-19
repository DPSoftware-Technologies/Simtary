#include "stream/StreamHost.h"

#include "wiArguments.h"
#include "wiBacklog.h"

#ifdef SIMTARY_HAS_BHS

#include "bhs.h"

#include "imgui.h"
#include "wiApplication.h"
#include "wiEventHandler.h"
#include "wiGraphics.h"
#include "wiGraphicsDevice.h"
#include "wiInput.h"
#include "wiRenderer.h"

#include <algorithm>
#include <mutex>
#include <vector>

using namespace wi::graphics;

namespace st::stream {

namespace {

constexpr int kRing = 3;

// USB HID usage id (what the browser sends, keycodes.md) -> the engine's button, or BUTTON_NONE.
wi::input::BUTTON ButtonFromHid (int hid) {
    using namespace wi::input;
    if (hid >= 4 && hid <= 29) return BUTTON('A' + (hid - 4));
    if (hid >= 30 && hid <= 38) return BUTTON('1' + (hid - 30));
    if (hid == 39) return BUTTON('0');
    if (hid >= 58 && hid <= 69) return BUTTON(KEYBOARD_BUTTON_F1 + (hid - 58));
    switch (hid) {
        case 40: return KEYBOARD_BUTTON_ENTER;
        case 41: return KEYBOARD_BUTTON_ESCAPE;
        case 42: return KEYBOARD_BUTTON_BACKSPACE;
        case 43: return KEYBOARD_BUTTON_TAB;
        case 44: return KEYBOARD_BUTTON_SPACE;
        case 79: return KEYBOARD_BUTTON_RIGHT;
        case 80: return KEYBOARD_BUTTON_LEFT;
        case 81: return KEYBOARD_BUTTON_DOWN;
        case 82: return KEYBOARD_BUTTON_UP;
        case 224: return KEYBOARD_BUTTON_LCONTROL;
        case 225: return KEYBOARD_BUTTON_LSHIFT;
        case 228: return KEYBOARD_BUTTON_RCONTROL;
        case 229: return KEYBOARD_BUTTON_RSHIFT;
        default: return BUTTON_NONE;
    }
}

} // namespace

struct StreamHost::Impl {
    bhs_node* node = nullptr;
    wi::Application* app = nullptr;
    Options options;

    std::mutex lock;
    std::vector<bhs_input_event> pending;   // filled on bhs's threads, drained in Update
    bool viewerJoined = false;
    bool viewerLeft = false;

    Texture ring[kRing];
    int slot = 0;
    uint64_t copied = 0;
    TextureDesc ringDesc;

    // virtual pointer, in video pixels (the browser sends absolute 0..65535)
    float pointerX = 0.0f, pointerY = 0.0f;
    bool pointerKnown = false;

    static void OnInput (void* user, const bhs_input_event* e) {
        auto* self = static_cast<Impl*>(user);
        std::scoped_lock l(self->lock);
        if (self->pending.size() < 4096) self->pending.push_back(*e);
    }
    static void OnViewer (void* user, uint32_t, int joined) {
        auto* self = static_cast<Impl*>(user);
        std::scoped_lock l(self->lock);
        if (joined) self->viewerJoined = true; else self->viewerLeft = true;
    }

    bool EnsureRing (const TextureDesc& backbuffer) {
        if (ring[0].IsValid() && ringDesc.width == backbuffer.width && ringDesc.height == backbuffer.height &&
            ringDesc.format == backbuffer.format) {
            return true;
        }
        GraphicsDevice* device = GetDevice();
        TextureDesc desc;
        desc.width = backbuffer.width;
        desc.height = backbuffer.height;
        desc.format = backbuffer.format;
        desc.mip_levels = 1;
        desc.array_size = 1;
        desc.usage = Usage::DEFAULT;
        // RENDER_TARGET is not used, but D3D11 refuses to open a shared D3D12 texture without it
        // (bhs/tools/share_test prints the matrix).
        desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::RENDER_TARGET;
        desc.misc_flags = ResourceMiscFlag::SHARED;
        // Rests in the swapchain state (= COMMON on D3D12), which is what another API expects.
        desc.layout = ResourceState::SWAPCHAIN;
        for (int i = 0; i < kRing; ++i) {
            ring[i] = Texture();
            if (!device->CreateTexture(&desc, nullptr, &ring[i]) || ring[i].shared_handle == nullptr) {
                wi::backlog::post("Stream: cannot create a shared frame texture (" + std::to_string(desc.width) + "x" +
                                  std::to_string(desc.height) + ")", wi::backlog::LogLevel::Error);
                ring[0] = Texture();
                return false;
            }
            device->SetName(&ring[i], "StreamHost::frame");
        }
        ringDesc = desc;
        slot = 0;
        copied = 0;
        wi::backlog::post("Stream: shared frame ring " + std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                          ", format " + std::to_string(int(desc.format)));
        return true;
    }

    void OnComposed (CommandList cmd, const Texture& backbuffer) {
        if (node == nullptr) return;
        if (!bhs_node_has_viewer(node)) {
            copied = 0;
            return;
        }
        if (!EnsureRing(backbuffer.desc)) return;
        GraphicsDevice* device = GetDevice();

        Texture& dst = ring[slot];
        wi::renderer::PushBarrier(GPUBarrier::Image(&backbuffer, backbuffer.desc.layout, ResourceState::COPY_SRC));
        wi::renderer::PushBarrier(GPUBarrier::Image(&dst, dst.desc.layout, ResourceState::COPY_DST));
        wi::renderer::FlushBarriers(cmd);
        device->CopyResource(&dst, &backbuffer, cmd);
        wi::renderer::PushBarrier(GPUBarrier::Image(&dst, ResourceState::COPY_DST, dst.desc.layout));
        wi::renderer::PushBarrier(GPUBarrier::Image(&backbuffer, ResourceState::COPY_SRC, backbuffer.desc.layout));
        wi::renderer::FlushBarriers(cmd);

        slot = (slot + 1) % kRing;
        ++copied;
        // ring[slot] is the oldest copy now: written kRing - 1 frames ago, so the GPU is done with it.
        if (copied >= uint64_t(kRing)) {
            bhs_node_submit_shared(node, ring[slot].shared_handle, ringDesc.width, ringDesc.height);
        }
    }

    void Apply (const bhs_input_event& e) {
        using namespace wi::input;
        ImGuiIO& io = ImGui::GetIO();
        const float w = float(options.width), h = float(options.height);
        switch (e.kind) {
            case 1: case 2: {
                const BUTTON b = ButtonFromHid(e.a);
                if (b != BUTTON_NONE) InjectButton(b, e.kind == 1);
                break;
            }
            case 4: {   // absolute pointer, 0..65535 of the video
                const float x = float(e.a) / 65535.0f * w, y = float(e.b) / 65535.0f * h;
                pointerX = x; pointerY = y; pointerKnown = true;
                io.AddMousePosEvent(x, y);
                break;
            }
            case 3: {   // relative pointer (pointer-lock viewers)
                pointerX = std::clamp(pointerX + float(e.a), 0.0f, w);
                pointerY = std::clamp(pointerY + float(e.b), 0.0f, h);
                io.AddMousePosEvent(pointerX, pointerY);
                break;
            }
            case 5: {   // 0 left, 1 middle, 2 right
                const bool down = e.b != 0;
                if (e.a == 0) { InjectButton(MOUSE_BUTTON_LEFT, down); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down); }
                else if (e.a == 1) { InjectButton(MOUSE_BUTTON_MIDDLE, down); io.AddMouseButtonEvent(ImGuiMouseButton_Middle, down); }
                else if (e.a == 2) { InjectButton(MOUSE_BUTTON_RIGHT, down); io.AddMouseButtonEvent(ImGuiMouseButton_Right, down); }
                break;
            }
            case 6: {   // browser deltaY is positive when scrolling down; ImGui / the engine want up positive
                const float steps = -float(e.b) / 100.0f;
                io.AddMouseWheelEvent(0.0f, steps);
                AddMouseScrollEvent(steps);
                break;
            }
            default: break;   // gamepad, touch: not wired yet
        }
    }
};

StreamHost& StreamHost::Get () {
    static StreamHost host;
    return host;
}

void StreamHost::ParseCommandLine () {
    options_ = Options();
    std::string value;
    const bool flag = wi::arguments::HasArgument("--stream");
    const bool valued = wi::arguments::GetArgumentValue("--stream=", value);
    if (!flag && !valued) return;
    options_.enabled = true;
    if (valued) {
        const int p = atoi(value.c_str());
        if (p > 0 && p < 65536) options_.port = uint16_t(p);
    }
    if (wi::arguments::GetArgumentValue("--stream-size=", value)) {
        unsigned w = 0, h = 0;
        if (sscanf(value.c_str(), "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64) {
            options_.width = w & ~1u;
            options_.height = h & ~1u;
        }
    }
    if (wi::arguments::GetArgumentValue("--stream-fps=", value)) options_.fps = uint32_t(std::clamp(atoi(value.c_str()), 5, 240));
    if (wi::arguments::GetArgumentValue("--stream-kbps=", value)) options_.kbps = uint32_t(std::clamp(atoi(value.c_str()), 300, 100000));
    if (wi::arguments::GetArgumentValue("--stream-bind=", value) && !value.empty()) options_.bind = value;
    options_.showWindow = wi::arguments::HasArgument("--stream-window");
}

void StreamHost::Initialize (wi::Application& app) {
    if (!options_.enabled || running_) return;
    impl_ = new Impl();
    impl_->app = &app;
    impl_->options = options_;

    bhs_config cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.port = options_.port;
    cfg.bind_address = options_.bind.c_str();
    cfg.width = options_.width;
    cfg.height = options_.height;
    cfg.fps = options_.fps;
    cfg.kbps = options_.kbps;
    cfg.user = impl_;
    cfg.on_input = &Impl::OnInput;
    cfg.on_viewer = &Impl::OnViewer;
    char error[256] = {};
    impl_->node = bhs_node_start(&cfg, error, sizeof(error));
    if (impl_->node == nullptr) {
        wi::backlog::post(std::string("Stream: cannot start - ") + error, wi::backlog::LogLevel::Error);
        delete impl_;
        impl_ = nullptr;
        return;
    }
    // Pace the loop to the stream: no point rendering faster than the encoder takes frames.
    wi::eventhandler::SetVSync(false);
    app.setFrameRateLock(true);
    app.setTargetFrameRate(float(options_.fps));
    app.onFrameComposed = [this](CommandList cmd, const Texture& backbuffer) { impl_->OnComposed(cmd, backbuffer); };
    running_ = true;
    wi::backlog::post("Stream: " + std::to_string(options_.width) + "x" + std::to_string(options_.height) + " @ " +
                      std::to_string(options_.fps) + " fps on ws://" + options_.bind + ":" + std::to_string(options_.port) +
                      " - open bhs/web/index.html through a local web server and press connect");
}

void StreamHost::Update () {
    if (!running_) return;
    std::vector<bhs_input_event> events;
    bool joined = false, left = false;
    {
        std::scoped_lock l(impl_->lock);
        events.swap(impl_->pending);
        joined = impl_->viewerJoined;
        left = impl_->viewerLeft;
        impl_->viewerJoined = impl_->viewerLeft = false;
    }
    if (left) {
        // A viewer that vanished with keys held must not leave the vehicle driving itself.
        for (int b = int(wi::input::BUTTON_NONE) + 1; b < int(wi::input::BUTTON_ENUM_SIZE); ++b) {
            wi::input::InjectButton(wi::input::BUTTON(b), false);
        }
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    }
    (void)joined;
    for (const bhs_input_event& e : events) impl_->Apply(e);
}

bool StreamHost::HasViewer () const {
    return running_ && impl_ != nullptr && bhs_node_has_viewer(impl_->node) != 0;
}

void StreamHost::Shutdown () {
    if (!running_) return;
    impl_->app->onFrameComposed = nullptr;
    bhs_node_stop(impl_->node);
    delete impl_;
    impl_ = nullptr;
    running_ = false;
}

} // namespace st::stream

#else // no bhs build: --stream says why it does nothing

namespace st::stream {

struct StreamHost::Impl {};

StreamHost& StreamHost::Get () {
    static StreamHost host;
    return host;
}
void StreamHost::ParseCommandLine () {
    options_ = Options();
    if (wi::arguments::HasArgument("--stream") || wi::arguments::HasArgument("--stream=")) {
        wi::backlog::post("Stream: this build has no BHS (run Simtary/bhs/tools/build_bhs.ps1, then reconfigure)",
                          wi::backlog::LogLevel::Warning);
    }
}
void StreamHost::Initialize (wi::Application&) {}
void StreamHost::Update () {}
void StreamHost::Shutdown () {}
bool StreamHost::HasViewer () const { return false; }

} // namespace st::stream

#endif
