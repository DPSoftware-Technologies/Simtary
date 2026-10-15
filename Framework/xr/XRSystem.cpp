#include "xr/XRSystem.h"

#include "dev/DevBuild.h"
#include "dev/EditorBridge.h"
#include "render/GraphicsAPI.h"

#include "wiArguments.h"
#include "wiBacklog.h"
#include "wiGraphicsDevice.h"
#include "wiHelper.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#if defined(SIMTARY_HAS_OPENXR)
#include "wiGraphicsDevice_DX12.h"   // d3d12.h + windows.h, which openxr_platform.h needs first
#include "wiImage.h"
#include "wiRenderPath3D.h"
#include "wiScene.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

namespace st::xr {

XRSystem& XRSystem::Get () {
    static XRSystem instance;
    return instance;
}

namespace {

void Log (const std::string& msg, wi::backlog::LogLevel level = wi::backlog::LogLevel::Default) {
    wi::backlog::post("XR: " + msg, level);
}

void RegisterBridge (XRSystem& xr) {
    if (!st::dev::IsDevBuild()) return;
    st::dev::EditorBridge::Get().Register("xr.status", "{} -> OpenXR runtime, headset, session state",
        [&xr](const st::json::Value&, std::string&) {
            const XRStatus& s = xr.Status();
            st::json::Value o = st::json::Value::Object();
            o.Set("compiled", s.compiled);
            o.Set("requested", s.requested);
            o.Set("instance", s.instance);
            o.Set("system", s.system);
            o.Set("session", s.session);
            o.Set("running", s.running);
            o.Set("adapterMatches", s.adapterMatches);
            o.Set("runtime", s.runtime);
            o.Set("systemName", s.systemName);
            o.Set("state", s.state);
            o.Set("error", s.error);
            o.Set("swapchainFormat", s.swapchainFormat);
            o.Set("eyeWidth", s.eyeWidth);
            o.Set("eyeHeight", s.eyeHeight);
            o.Set("maxEyeWidth", s.maxEyeWidth);
            o.Set("maxEyeHeight", s.maxEyeHeight);
            o.Set("framesSubmitted", uint64_t(s.framesSubmitted));
            o.Set("framesWithImage", uint64_t(s.framesWithImage));
            return o;
        });
    st::dev::EditorBridge::Get().Register("xr.recenter", "{} -> the head's current position and heading become the seat",
        [&xr](const st::json::Value&, std::string&) {
            xr.Recenter();
            st::json::Value o = st::json::Value::Object();
            o.Set("queued", true);
            return o;
        });
    st::dev::EditorBridge::Get().Register("xr.capture", "{path} -> writes <path>_L.png / _R.png after the next headset frame",
        [&xr](const st::json::Value& a, std::string& err) {
            if (!a.Has("path")) {
                err = "path required";
                return st::json::Value();
            }
            xr.CaptureEyes(a["path"].AsString());
            st::json::Value o = st::json::Value::Object();
            o.Set("queued", true);
            return o;
        });
}

} // namespace

#if defined(SIMTARY_HAS_OPENXR)

using namespace wi::graphics;

namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr double kSystemPollSeconds = 2.0;
constexpr uint32_t kEyes = 2;

double Now () {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char* StateName (XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "UNKNOWN";
    }
}

// The swapchain formats we can fill. sRGB on purpose: the eye path's output is already
// display-encoded (the tonemapper applies the sRGB curve), and an sRGB swapchain is what
// tells the compositor those bytes are encoded. The copy goes through a plain UNORM target
// of the same byte layout, so nothing re-encodes them on the way. A UNORM swapchain would
// be read as LINEAR and come out washed out.
struct FormatChoice {
    DXGI_FORMAT swapchain;
    Format image;     // the swapchain image as the engine sees it
    Format target;    // the intermediate the eye is drawn into, same bytes, no sRGB
    const char* name;
};
constexpr FormatChoice kFormats[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Format::R8G8B8A8_UNORM_SRGB, Format::R8G8B8A8_UNORM, "R8G8B8A8_UNORM_SRGB" },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, Format::B8G8R8A8_UNORM_SRGB, Format::B8G8R8A8_UNORM, "B8G8R8A8_UNORM_SRGB" },
};

// OpenXR is right-handed (+X right, +Y up, -Z forward), the engine left-handed with +Z
// forward: mirror Z. For a rotation that is (x, y, z, w) -> (-x, -y, z, w).
XMVECTOR ToEngine (const XrVector3f& p) { return XMVectorSet(p.x, p.y, -p.z, 0.0f); }
XMVECTOR ToEngine (const XrQuaternionf& q) { return XMVectorSet(-q.x, -q.y, q.z, q.w); }

// Off-centre perspective from the four half-angles, reverse-Z like the engine's own
// (CameraComponent::UpdateCamera passes far as "near"). With the near argument = zFar the
// frustum edges have to be given at that distance; x/z then maps to NDC independently of it.
XMMATRIX EyeProjection (const XrFovf& fov, float zNear, float zFar) {
    const float l = tanf(fov.angleLeft) * zFar;
    const float r = tanf(fov.angleRight) * zFar;
    const float d = tanf(fov.angleDown) * zFar;
    const float u = tanf(fov.angleUp) * zFar;
    return XMMatrixPerspectiveOffCenterLH(l, r, d, u, zFar, zNear);
}

} // namespace

struct XRSystem::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    double nextSystemPoll = 0.0;
    bool noHeadsetLogged = false;

    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<Texture> images;          // the runtime's images, wrapped (copy target only)
        Texture target;                       // the eye path's output, re-drawn at UNORM
        std::unique_ptr<wi::RenderPath3D> path;
        wi::scene::CameraComponent camera;
        uint32_t acquired = UINT32_MAX;       // image index held this frame
    };
    Eye eyes[kEyes];
    uint32_t width = 0, height = 0;
    bool eyesReady = false;

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    XrView views[kEyes] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    bool frameBegun = false;
    bool viewsValid = false;

    // Recenter: the head pose (engine space) that counts as "sitting straight in the seat".
    bool recenterPending = true;
    XMFLOAT3 seatPos = XMFLOAT3(0, 0, 0);
    float seatYaw = 0.0f;

    XRStatus& status;
    explicit Impl (XRStatus& s) : status(s) {}

    // A failed call: remember it and say so. False when r is a failure.
    bool Check (XrResult r, const char* what) {
        if (XR_SUCCEEDED(r)) return true;
        char text[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, r, text))) {
            snprintf(text, sizeof(text), "XrResult %d", int(r));
        }
        status.error = std::string(what) + ": " + text;
        Log(status.error, wi::backlog::LogLevel::Warning);
        return false;
    }

    GraphicsDevice_DX12* Device () const {
        // Safe without RTTI: Initialize only gets this far on a DirectX 12 run.
        return static_cast<GraphicsDevice_DX12*>(wi::graphics::GetDevice());
    }

    bool CreateInstance (const std::string& appName) {
        uint32_t count = 0;
        if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
                   "no OpenXR runtime (xrEnumerateInstanceExtensionProperties)")) {
            return false;
        }
        std::vector<XrExtensionProperties> exts(count, { XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
        bool hasD3D12 = false;
        for (const XrExtensionProperties& e : exts) {
            if (strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0) hasD3D12 = true;
        }
        if (!hasD3D12) {
            status.error = "the active OpenXR runtime has no D3D12 support (XR_KHR_D3D12_enable)";
            Log(status.error, wi::backlog::LogLevel::Warning);
            return false;
        }

        const char* enabled[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
        strncpy(ci.applicationInfo.applicationName, appName.c_str(), XR_MAX_APPLICATION_NAME_SIZE - 1);
        ci.applicationInfo.applicationVersion = 1;
        strncpy(ci.applicationInfo.engineName, "Simtary", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.engineVersion = 1;
        // 1.0 rather than the header's current version: every shipping runtime accepts
        // it, and nothing here needs 1.1.
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = 1;
        ci.enabledExtensionNames = enabled;
        if (!Check(xrCreateInstance(&ci, &instance), "xrCreateInstance")) return false;

        XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &props))) {
            status.runtime = std::string(props.runtimeName) + " " +
                std::to_string(XR_VERSION_MAJOR(props.runtimeVersion)) + "." +
                std::to_string(XR_VERSION_MINOR(props.runtimeVersion)) + "." +
                std::to_string(XR_VERSION_PATCH(props.runtimeVersion));
        }
        status.instance = true;
        Log("runtime " + status.runtime);
        return true;
    }

    // Finds the headset. A missing one is normal (not plugged in, SteamVR still
    // starting), so it is retried from Update rather than treated as a failure.
    bool TryGetSystem () {
        nextSystemPoll = Now() + kSystemPollSeconds;
        XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        const XrResult r = xrGetSystem(instance, &gi, &systemId);
        if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            status.state = "NO_HEADSET";
            if (!noHeadsetLogged) Log("no headset connected - retrying every 2 s");
            noHeadsetLogged = true;
            return false;
        }
        if (!Check(r, "xrGetSystem")) return false;

        XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
        xrGetSystemProperties(instance, systemId, &sp);
        status.systemName = sp.systemName;
        status.maxEyeWidth = sp.graphicsProperties.maxSwapchainImageWidth;
        status.maxEyeHeight = sp.graphicsProperties.maxSwapchainImageHeight;

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(instance, systemId, kViewConfig, 0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        if (viewCount != kEyes ||
            XR_FAILED(xrEnumerateViewConfigurationViews(instance, systemId, kViewConfig, viewCount,
                                                        &viewCount, views.data()))) {
            status.error = "the headset does not report a stereo view configuration";
            Log(status.error, wi::backlog::LogLevel::Warning);
            return false;
        }
        width = views[0].recommendedImageRectWidth;
        height = views[0].recommendedImageRectHeight;
        status.eyeWidth = width;
        status.eyeHeight = height;
        status.system = true;
        Log("headset " + status.systemName + ", " + std::to_string(width) + "x" +
            std::to_string(height) + " per eye");
        return CreateSession();
    }

    bool CreateSession () {
        GraphicsDevice_DX12* dx12 = Device();
        if (dx12 == nullptr) {
            status.error = "no graphics device";
            return false;
        }
        ID3D12Device* d3d = dx12->GetD3D12Device();

        // Mandatory before xrCreateSession, and the one check that matters on a laptop:
        // the headset is wired to ONE GPU, and the session must live on that adapter.
        PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nullptr;
        if (!Check(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
                   "xrGetInstanceProcAddr(xrGetD3D12GraphicsRequirementsKHR)")) {
            return false;
        }
        XrGraphicsRequirementsD3D12KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        if (!Check(getRequirements(instance, systemId, &req), "xrGetD3D12GraphicsRequirementsKHR")) {
            return false;
        }
        const LUID luid = d3d->GetAdapterLuid();
        status.adapterMatches = memcmp(&luid, &req.adapterLuid, sizeof(LUID)) == 0;
        if (!status.adapterMatches) {
            status.error = "the headset is on a different GPU than the engine - start with the "
                           "GPU it is wired to (nvidiagpu / amdgpu / igpu)";
            Log(status.error, wi::backlog::LogLevel::Warning);
            return false;
        }

        XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        binding.device = d3d;
        binding.queue = dx12->GetGraphicsCommandQueue();
        XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
        sci.next = &binding;
        sci.systemId = systemId;
        if (!Check(xrCreateSession(instance, &sci, &session), "xrCreateSession")) return false;

        XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;   // seated: origin where the head started
        rs.poseInReferenceSpace.orientation.w = 1.0f;
        Check(xrCreateReferenceSpace(session, &rs, &localSpace), "xrCreateReferenceSpace(LOCAL)");

        status.session = true;
        status.error.clear();
        Log("session created");
        eyesReady = CreateEyes();
        return true;
    }

    // One swapchain, one render path and one camera per eye.
    bool CreateEyes () {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        const FormatChoice* choice = nullptr;
        for (const FormatChoice& f : kFormats) {          // our preference order
            for (int64_t offered : formats) {
                if (offered == int64_t(f.swapchain)) { choice = &f; break; }
            }
            if (choice != nullptr) break;
        }
        if (choice == nullptr) {
            status.error = "the runtime offers no 8-bit sRGB swapchain format";
            Log(status.error, wi::backlog::LogLevel::Warning);
            return false;
        }
        status.swapchainFormat = choice->name;

        GraphicsDevice_DX12* dx12 = Device();
        for (uint32_t i = 0; i < kEyes; ++i) {
            Eye& eye = eyes[i];
            XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            sc.format = int64_t(choice->swapchain);
            sc.sampleCount = 1;
            sc.width = width;
            sc.height = height;
            sc.faceCount = 1;
            sc.arraySize = 1;
            sc.mipCount = 1;
            if (!Check(xrCreateSwapchain(session, &sc, &eye.swapchain), "xrCreateSwapchain")) return false;

            uint32_t n = 0;
            xrEnumerateSwapchainImages(eye.swapchain, 0, &n, nullptr);
            std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
            if (!Check(xrEnumerateSwapchainImages(eye.swapchain, n, &n,
                           reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
                       "xrEnumerateSwapchainImages")) {
                return false;
            }
            // The spec hands colour images over in RENDER_TARGET and wants them back in it.
            TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = choice->image;
            desc.bind_flags = BindFlag::RENDER_TARGET;
            desc.layout = ResourceState::RENDERTARGET;
            eye.images.resize(n);
            for (uint32_t k = 0; k < n; ++k) {
                dx12->WrapNativeTexture(imgs[k].texture, desc, &eye.images[k]);
            }

            TextureDesc td;
            td.width = width;
            td.height = height;
            td.format = choice->target;
            td.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE;
            td.layout = ResourceState::COPY_SRC;
            dx12->CreateTexture(&td, nullptr, &eye.target);
            dx12->SetName(&eye.target, i == 0 ? "XR eye target L" : "XR eye target R");

            eye.path = std::make_unique<wi::RenderPath3D>();
            eye.path->camera = &eye.camera;
            // Like the editor's extra views: the scene is stepped by the game, and the game
            // path already simulates particles / refreshes probes this frame - a second path
            // running with a live delta would do it again.
            eye.path->setSceneUpdateEnabled(false);
            eye.path->setOcclusionCullingEnabled(false);
            eye.path->setMSAASampleCount(1);
            eye.path->init(width, height, 96.0f);
            eye.path->Start();
        }
        Log(std::string("swapchains ") + choice->name + ", " + std::to_string(eyes[0].images.size()) +
            " images per eye");
        return true;
    }

    void DestroyEyes () {
        if (eyesReady || eyes[0].path || eyes[1].path) {
            if (GetDevice() != nullptr) GetDevice()->WaitForGPU();
        }
        for (Eye& eye : eyes) {
            if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
            eye.swapchain = XR_NULL_HANDLE;
            eye.images.clear();
            eye.target = {};
            eye.path.reset();
            eye.acquired = UINT32_MAX;
        }
        eyesReady = false;
    }

    void DestroySession () {
        DestroyEyes();
        if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
        if (session != XR_NULL_HANDLE) xrDestroySession(session);
        localSpace = XR_NULL_HANDLE;
        session = XR_NULL_HANDLE;
        state = XR_SESSION_STATE_UNKNOWN;
        frameBegun = false;
        viewsValid = false;
        status.session = false;
        status.running = false;
    }

    void DestroyAll () {
        DestroySession();
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        systemId = XR_NULL_SYSTEM_ID;
        status.instance = false;
        status.system = false;
    }

    void OnStateChanged (XrSessionState next) {
        state = next;
        status.state = StateName(next);
        Log(std::string("session ") + status.state);
        switch (next) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = kViewConfig;
                status.running = Check(xrBeginSession(session, &bi), "xrBeginSession");
                recenterPending = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                Check(xrEndSession(session), "xrEndSession");
                status.running = false;
                break;
            case XR_SESSION_STATE_EXITING:        // the user quit the app from the runtime
            case XR_SESSION_STATE_LOSS_PENDING:   // headset unplugged, runtime restarting
                DestroySession();
                status.system = false;            // find it again from the top
                noHeadsetLogged = false;
                nextSystemPoll = Now() + kSystemPollSeconds;
                break;
            default:
                break;
        }
    }

    void PollEvents () {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        while (instance != XR_NULL_HANDLE && xrPollEvent(instance, &ev) == XR_SUCCESS) {
            switch (ev.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                    if (e.session == session) OnStateChanged(e.state);
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    status.error = "OpenXR runtime is going away (instance loss pending)";
                    Log(status.error, wi::backlog::LogLevel::Warning);
                    DestroyAll();
                    return;
                default:
                    break;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    // Top of the frame: wait for the headset's slot, open the frame, and ask where the
    // eyes will be when this frame is displayed.
    void BeginFrame () {
        viewsValid = false;
        frameState = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
        if (!Check(xrWaitFrame(session, &waitInfo, &frameState), "xrWaitFrame")) return;
        XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!Check(xrBeginFrame(session, &bi), "xrBeginFrame")) return;
        frameBegun = true;
        if (!frameState.shouldRender || !eyesReady || localSpace == XR_NULL_HANDLE) return;

        XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = kViewConfig;
        li.displayTime = frameState.predictedDisplayTime;
        li.space = localSpace;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        uint32_t n = 0;
        views[0] = { XR_TYPE_VIEW };
        views[1] = { XR_TYPE_VIEW };
        if (!Check(xrLocateViews(session, &li, &vs, kEyes, &n, views), "xrLocateViews")) return;
        viewsValid = n == kEyes && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        if (viewsValid && recenterPending) {
            const XMVECTOR c = XMVectorScale(XMVectorAdd(ToEngine(views[0].pose.position),
                                                         ToEngine(views[1].pose.position)), 0.5f);
            XMStoreFloat3(&seatPos, c);
            const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), ToEngine(views[0].pose.orientation));
            seatYaw = atan2f(XMVectorGetX(fwd), XMVectorGetZ(fwd));
            recenterPending = false;
            Log("recentred");
        }
    }

    void RenderEyes (float dt, bool eyesOwnScene, const std::function<void(wi::RenderPath3D&)>& configure) {
        if (!frameBegun || !viewsValid) return;
        GraphicsDevice_DX12* device = Device();

        // The anchor: where the game camera is and which way it faces. Heading only - the
        // head supplies pitch and roll, and a pitched anchor would tilt the horizon.
        const wi::scene::CameraComponent& game = wi::scene::GetCamera();
        const XMVECTOR anchorPos = XMLoadFloat3(&game.Eye);
        const float yaw = atan2f(game.At.x, game.At.z);
        const XMMATRIX anchorRot = XMMatrixRotationY(yaw);
        const XMVECTOR anchorQ = XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f);
        // The head relative to the seat (recenter): undo the seat's heading, then the
        // anchor's heading puts it on the vehicle.
        const XMVECTOR seat = XMLoadFloat3(&seatPos);
        const XMMATRIX unSeat = XMMatrixRotationY(-seatYaw);
        const XMVECTOR unSeatQ = XMQuaternionRotationRollPitchYaw(0.0f, -seatYaw, 0.0f);

        for (uint32_t i = 0; i < kEyes; ++i) {
            Eye& eye = eyes[i];
            const XrView& view = views[i];

            wi::RenderPath3D& path = *eye.path;
            path.camera = &eye.camera;
            // FIRST: PreUpdate saves the camera as "previous" (TAA, cloud reprojection). It has
            // to see last frame's eye, not the game camera this frame's copy starts from.
            path.PreUpdate();
            // Left eye live when the desktop path is off: it then owns the frame's particle
            // simulation and refreshes (frame delta), the right eye just draws the result.
            const bool live = eyesOwnScene && i == 0;
            path.setSceneUpdateEnabled(live);
            path.setSceneUpdateExternal(live);

            const XMVECTOR local = XMVector3Transform(XMVectorSubtract(ToEngine(view.pose.position), seat), unSeat);
            const XMVECTOR headPos = XMVector3Transform(local, anchorRot);
            const XMVECTOR headQ = XMQuaternionMultiply(ToEngine(view.pose.orientation), unSeatQ);
            const XMVECTOR rot = XMQuaternionMultiply(headQ, anchorQ);
            wi::scene::CameraComponent& cam = eye.camera;
            cam = game;   // near/far, exposure, aperture... from the game camera
            XMStoreFloat3(&cam.Eye, XMVectorAdd(anchorPos, headPos));
            XMStoreFloat3(&cam.At, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), rot));
            XMStoreFloat3(&cam.Up, XMVector3Rotate(XMVectorSet(0, 1, 0, 0), rot));
            cam.width = float(width);
            cam.height = float(height);
            cam.SetCustomProjectionEnabled(true);
            XMStoreFloat4x4(&cam.Projection, EyeProjection(view.fov, cam.zNearP, cam.zFarP));
            cam.UpdateCamera();   // View / VP / frustum for this eye before anything reads them

            if (configure) configure(path);
            // Comfort and correctness in a headset: these are screen effects of a camera
            // that is not there, and FSR2 needs a jitter the custom projection does not get.
            path.setMotionBlurEnabled(false);
            path.setDepthOfFieldEnabled(false);
            path.setChromaticAberrationEnabled(false);
            path.setFSR2Enabled(false);
            path.init(width, height, 96.0f);
            path.Update(dt);
            path.PostUpdate();
            cam.UpdateCamera();
            path.PreRender();
            path.Render();
            path.PostRender();

            const Texture* out = path.GetLastPostprocessRT();
            if (out == nullptr || !out->IsValid()) continue;

            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t index = 0;
            if (!Check(xrAcquireSwapchainImage(eye.swapchain, &ai, &index), "xrAcquireSwapchainImage")) continue;
            XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (!Check(xrWaitSwapchainImage(eye.swapchain, &waitInfo), "xrWaitSwapchainImage")) {
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(eye.swapchain, &ri);
                continue;
            }
            eye.acquired = index;
            Texture& image = eye.images[index];

            // Re-draw the eye's output at UNORM (bytes unchanged: it is already encoded),
            // then copy it into the runtime's image - the copy is format-family-only, so
            // the sRGB image receives exactly those bytes.
            CommandList cmd = device->BeginCommandList();
            Viewport vp;
            vp.width = float(width);
            vp.height = float(height);
            device->BindViewports(1, &vp, cmd);
            const RenderPassImage rp = RenderPassImage::RenderTarget(&eye.target, RenderPassImage::LoadOp::DONTCARE,
                RenderPassImage::StoreOp::STORE, ResourceState::COPY_SRC, ResourceState::COPY_SRC);
            device->RenderPassBegin(&rp, 1, cmd);
            wi::image::Params fx;
            fx.enableFullScreen();
            fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
            wi::image::Draw(out, fx, cmd);
            device->RenderPassEnd(cmd);

            const GPUBarrier toCopy = GPUBarrier::Image(&image, ResourceState::RENDERTARGET, ResourceState::COPY_DST);
            device->Barrier(&toCopy, 1, cmd);
            static_cast<GraphicsDevice*>(device)->CopyTexture(&image, 0, 0, 0, 0, 0, &eye.target, 0, 0, cmd); // base: defaults
            const GPUBarrier back = GPUBarrier::Image(&image, ResourceState::COPY_DST, ResourceState::RENDERTARGET);
            device->Barrier(&back, 1, cmd);
        }
    }

    // After the engine submitted the frame: the copies are on the queue, so the images can
    // go back to the runtime and the frame can be closed.
    void EndFrame () {
        if (!frameBegun) return;
        bool haveEyes = viewsValid;
        for (Eye& eye : eyes) {
            if (eye.acquired == UINT32_MAX) {
                haveEyes = false;
                continue;
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            Check(xrReleaseSwapchainImage(eye.swapchain, &ri), "xrReleaseSwapchainImage");
            eye.acquired = UINT32_MAX;
        }

        XrCompositionLayerProjectionView pv[kEyes] = {
            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[1] = {};
        if (haveEyes) {
            for (uint32_t i = 0; i < kEyes; ++i) {
                pv[i].pose = views[i].pose;      // the pose the eye was rendered from
                pv[i].fov = views[i].fov;
                pv[i].subImage.swapchain = eyes[i].swapchain;
                pv[i].subImage.imageRect.offset = { 0, 0 };
                pv[i].subImage.imageRect.extent = { int32_t(width), int32_t(height) };
                pv[i].subImage.imageArrayIndex = 0;
            }
            layer.space = localSpace;
            layer.viewCount = kEyes;
            layer.views = pv;
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        }

        XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
        ei.displayTime = frameState.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = haveEyes ? 1 : 0;
        ei.layers = haveEyes ? layers : nullptr;
        if (Check(xrEndFrame(session, &ei), "xrEndFrame")) {
            ++status.framesSubmitted;
            if (haveEyes) ++status.framesWithImage;
        }
        frameBegun = false;
        viewsValid = false;
    }
};

void XRSystem::Initialize (const std::string& appName) {
    status_.compiled = true;
    status_.requested = wi::arguments::HasArgument("--xr") || wi::arguments::HasArgument("xr");
    RegisterBridge(*this);
    if (!status_.requested) return;

    if (st::ActiveGraphicsAPI() != st::GraphicsAPI::DirectX12) {
        status_.error = "VR needs the DirectX 12 backend for now";
        Log(status_.error, wi::backlog::LogLevel::Warning);
        return;
    }
    impl_ = new Impl(status_);
    if (impl_->CreateInstance(appName)) impl_->TryGetSystem();
}

void XRSystem::Update () {
    if (impl_ == nullptr || impl_->instance == XR_NULL_HANDLE) return;
    if (!status_.system && Now() >= impl_->nextSystemPoll) impl_->TryGetSystem();
    impl_->PollEvents();
    // A frame left open (the engine skipped its render) is closed empty before the next.
    if (impl_->frameBegun) impl_->EndFrame();
    if (status_.running && impl_->session != XR_NULL_HANDLE) impl_->BeginFrame();
}

void XRSystem::RenderEyes (float dt, const std::function<void(wi::RenderPath3D&)>& configure) {
    if (impl_ != nullptr && status_.running) impl_->RenderEyes(dt, eyesOwnScene_, configure);
}

void XRSystem::Recenter () {
    if (impl_ != nullptr) impl_->recenterPending = true;
}

const wi::graphics::Texture* XRSystem::MirrorTexture () const {
    if (impl_ == nullptr || !impl_->eyesReady || !impl_->eyes[0].path) return nullptr;
    const Texture* t = impl_->eyes[0].path->GetLastPostprocessRT();
    return (t != nullptr && t->IsValid()) ? t : nullptr;
}

void XRSystem::EndFrame () {
    if (impl_ == nullptr || impl_->session == XR_NULL_HANDLE) return;
    const bool hadEyes = impl_->eyes[0].acquired != UINT32_MAX && impl_->eyes[1].acquired != UINT32_MAX;
    impl_->EndFrame();
    // After submission, so the targets hold this frame (the helper waits for the GPU).
    if (hadEyes && !capturePrefix_.empty()) {
        wi::helper::saveTextureToFile(impl_->eyes[0].target, capturePrefix_ + "_L.png");
        wi::helper::saveTextureToFile(impl_->eyes[1].target, capturePrefix_ + "_R.png");
        Log("captured eyes to " + capturePrefix_ + "_L/_R.png");
        capturePrefix_.clear();
    }
}

void XRSystem::Shutdown () {
    if (impl_ == nullptr) return;
    // Destroying a running session is allowed; xrEndSession is only legal in STOPPING.
    impl_->DestroyAll();
    delete impl_;
    impl_ = nullptr;
    Log("shut down");
}

#else // no OpenXR loader in this build

struct XRSystem::Impl {};

void XRSystem::Initialize (const std::string&) {
    status_.compiled = false;
    status_.requested = wi::arguments::HasArgument("--xr") || wi::arguments::HasArgument("xr");
    RegisterBridge(*this);
    if (status_.requested) {
        status_.error = "built without OpenXR (SIMTARY_ENABLE_OPENXR)";
        Log(status_.error, wi::backlog::LogLevel::Warning);
    }
}

void XRSystem::Update () {}
void XRSystem::RenderEyes (float, const std::function<void(wi::RenderPath3D&)>&) {}
void XRSystem::Recenter () {}
const wi::graphics::Texture* XRSystem::MirrorTexture () const { return nullptr; }
void XRSystem::EndFrame () {}
void XRSystem::Shutdown () {}

#endif

} // namespace st::xr
