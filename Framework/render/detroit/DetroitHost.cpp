// DetroitRenderer's host half: what st::App used to do with wi::renderer itself - the game
// path and its effects, the engine graphics options, the ImGui backend, composing the window.
#include "render/detroit/DetroitRenderer.h"
#include "render/detroit/DetroitDraw2D.h"

#include "io/SettingsManager.h"
#include "xr/XRSystem.h"

#include "wiApplication.h"
#include "wiRenderer.h"
#include "wiImage.h"
#include "wiBacklog.h"
#include "wiArguments.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace wi::graphics;

namespace st::render::detroit {

namespace {

// "mobile_minimal" (command line, or `adb shell am start ... --es args mobile_minimal` on
// Android): every optional render pass off. A bring-up tool for a new GPU - get a stable
// frame first, then turn things back on one at a time to find what breaks. (The scene half,
// weather effects, is st::App's: it is scene data, rewritten every frame.)
void ApplyMobileMinimal (wi::RenderPath3D& path) {
    path.setAO(wi::RenderPath3D::AO_DISABLED);
    path.setSSREnabled(false);
    path.setSSGIEnabled(false);
    path.setRaytracedReflectionsEnabled(false);
    path.setRaytracedDiffuseEnabled(false);
    path.setReflectionsEnabled(false);
    path.setShadowsEnabled(false);
    path.setBloomEnabled(false);
    path.setDepthOfFieldEnabled(false);
    path.setMotionBlurEnabled(false);
    path.setEyeAdaptionEnabled(false);
    path.setLensFlareEnabled(false);
    path.setLightShaftsEnabled(false);
    path.setVolumeLightsEnabled(false);
    path.setOcclusionCullingEnabled(false);
    path.setMeshBlendEnabled(false);
    path.setFSREnabled(false);
    path.setFXAAEnabled(false);
    path.setOutlineEnabled(false);
    path.setVisibilityComputeShadingEnabled(false);
    wi::renderer::SetDDGIEnabled(false);
    wi::renderer::SetSurfelGIEnabled(false);
    wi::renderer::SetTessellationEnabled(false);
    wi::renderer::SetVariableRateShadingClassification(false);
    wi::backlog::post("mobile_minimal: optional render passes off");
}

} // namespace

// ---------------------------------------------------------------------- lifetime ---

void DetroitRenderer::Initialize (wi::Application& app, Hooks hooks) {
    app_ = &app;
    hooks_ = std::move(hooks);

    // The device exists and the shader path has its backend subfolder appended
    //	(wi::Application::Initialize ran).
    lensFlare_.Init();
    projectors_.Init();
    lasers_.Init();

    gamePath_.init(app.canvas);
    gamePath_.Load();

    // Scenes own wi::scene::Scene::Update(dt) - they run it from sceneManager.Update(),
    // after st::InputSystem refresh, which native components read. RenderPath3D::Update()
    // would update the scene a second time (and too early). A second update per frame
    // swaps MeshComponent's so_pos/so_pre streamout views twice, which cancels out: GPU
    // skinning then always writes the same half and the previous-position half is never
    // written, so skinned meshes get garbage velocity (broken motion blur, TAA, FSR2).
    // "External" rather than disabled: a disabled path renders the scene as frozen - frame
    // delta time 0 (GPU particles never simulate), no probe / lightmap / decal refresh, no
    // acceleration structure updates for ray tracing or DDGI.
    gamePath_.setSceneUpdateExternal(true);

    // Project hook: last chance to configure the path (or activate one of your own)
    // before it goes live.
    if (hooks_.onPathSetup) hooks_.onPathSetup(gamePath_);

    // Only claim the path if the project did not activate one of its own.
    if (app.GetActivePath() == nullptr) app.ActivatePath(&gamePath_);

    // The projector pass rides on the render path's custom post process list, so it
    // has to be told which path after the setup hook has had its say. It only inserts
    // itself while there is something to draw.
    projectors_.Bind(gamePath_);
    // The path records the projector depth maps at the top of its post process chain,
    // where this frame's render data is already uploaded (see ProjectorRenderPath).
    gamePath_.projectors = &projectors_;

    // The laser pass rides on the same custom post process list, for the same reason
    // and at the same stage. It has no shadow maps, so it needs nothing from the path
    // beyond the hook.
    lasers_.Bind(gamePath_);

    LoadSettings();
    if (wi::arguments::HasArgument("mobile_minimal")) ApplyMobileMinimal(gamePath_);
}

void DetroitRenderer::LoadSettings () {
    // Push saved graphics options to the engine now, so the first frame reflects them.
    if (app_ != nullptr) graphics_.loadAndApply(st::SettingsManager::Get().GraphicsTag(), gamePath_, *app_);
    lensFlare_.LoadFrom(st::SettingsManager::Get().SubCompound("lensflare"));
    projectors_.LoadFrom(st::SettingsManager::Get().SubCompound("projectors"));
    lasers_.LoadFrom(st::SettingsManager::Get().SubCompound("lasers"));
}

void DetroitRenderer::SaveSettings () {
    graphics_.SaveTo(st::SettingsManager::Get().GraphicsTag());
    lensFlare_.SaveTo(st::SettingsManager::Get().SubCompound("lensflare"));
    projectors_.SaveTo(st::SettingsManager::Get().SubCompound("projectors"));
    lasers_.SaveTo(st::SettingsManager::Get().SubCompound("lasers"));
}

void DetroitRenderer::Shutdown () {
    ui_->Shutdown();
    gameImage_.external = nullptr;
}

// ------------------------------------------------------------------------- frame ---

void DetroitRenderer::Update (float dt) {
    // After the scene update: the scene's light system has refreshed the sun's direction
    // and the camera has already moved this frame, so the projected sun position matches
    // what the render path is about to draw. Followed entities carry this frame's
    // transform too, and the passes' constants must be in place before Render().
    // (st::App runs the CPU optics BEFORE this, so a beam or a virtual projector is
    // placed off a mirror where it will be drawn.)
    lensFlare_.Update(wi::scene::GetScene(), wi::scene::GetCamera(), dt);
    projectors_.Update(wi::scene::GetScene(), dt);
    lasers_.Update(wi::scene::GetScene(), dt);
}

void DetroitRenderer::RenderXREyes (float dt) {
    // The headset's two eyes: their own paths, configured like the game's.
    st::xr::XRSystem::Get().RenderEyes(dt, [this](wi::RenderPath3D& path) {
        path.colorspace = gamePath_.colorspace;
        graphics_.MirrorTo(path, false);
        path.setlayerMask(gamePath_.getLayerMask());
        path.custom_post_processes = gamePath_.custom_post_processes;
    });
}

void DetroitRenderer::Compose (CommandList cmd, ComposeMode mode) {
    // Editor mode draws the 3D through ImGui (each viewport panel samples its path's
    // render target), so the full-screen compose is skipped entirely: the dock host
    // covers the whole window anyway, and blitting under it would only cost a pass and
    // make the game's aspect ratio visible behind the panels for a frame after a resize.
    if (mode == ComposeMode::EditorOnly) {
        RenderImGui(cmd);
        return;
    }

    // VR: the window mirrors the left eye, letterboxed (the eye is taller than the window).
    if (mode == ComposeMode::XRMirror) {
        if (const wi::graphics::Texture* eye = st::xr::XRSystem::Get().MirrorTexture()) {
            const float cw = app_->canvas.GetLogicalWidth(), ch = app_->canvas.GetLogicalHeight();
            const float aspect = float(eye->desc.width) / float(std::max(1u, eye->desc.height));
            float h = ch, w = ch * aspect;
            if (w > cw) { w = cw; h = cw / aspect; }
            wi::image::Params fx((cw - w) * 0.5f, (ch - h) * 0.5f, w, h);
            fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
            wi::image::Draw(eye, fx, cmd);
        }
        if (hooks_.onCompose) hooks_.onCompose(cmd);
        RenderImGui(cmd);
        return;
    }

    // Under everything: a backdrop drawn before the engine composes the frame.
    if (hooks_.onPreCompose) hooks_.onPreCompose(cmd);

    app_->wi::Application::Compose(cmd);

    // Between the two: additively over the composed 3D frame, but under the UI, so
    // the flare never washes out ImGui windows.
    // The render path's depth copy goes with it: the flare is light that reached the
    // lens, so it has to be hidden by whatever is standing in front of the sun.
    lensFlare_.Draw(app_->canvas, cmd, &gamePath_.depthBuffer_Copy);

    if (hooks_.onCompose) hooks_.onCompose(cmd);

    RenderImGui(cmd);
}

// ---------------------------------------------------------------- ImGui backend ---

bool DetroitRenderer::CreateImGuiObjects () {
    // LoadShader reads from wi::renderer::GetShaderPath(), which by this point already has
    // the backend subfolder appended (Application::SetWindow ran in main before Initialize):
    // "<exe>/shaders/hlsl6/" on DX12, "<exe>/shaders/spirv/" on Vulkan. The build drops
    // ImGuiVS.cso / ImGuiPS.cso into those folders.
    const bool vsOK = wi::renderer::LoadShader(ShaderStage::VS, imguiVS_, "ImGuiVS.cso");
    const bool psOK = wi::renderer::LoadShader(ShaderStage::PS, imguiPS_, "ImGuiPS.cso");
    if (!vsOK || !psOK) {
        wi::backlog::post(
            "[ImGui] failed to load ImGuiVS.cso / ImGuiPS.cso from " +
            wi::renderer::GetShaderPath() +
            ". Build with dxc available, or copy the precompiled .cso files there. "
            "ImGui rendering will be disabled.",
            wi::backlog::LogLevel::Error);
    }

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    TextureDesc td;
    td.width      = width;
    td.height     = height;
    td.mip_levels = 1;
    td.array_size = 1;
    td.format     = Format::R8G8B8A8_UNORM;
    td.bind_flags = BindFlag::SHADER_RESOURCE;

    SubresourceData sd;
    sd.data_ptr    = pixels;
    sd.row_pitch   = width * GetFormatStride(td.format);
    sd.slice_pitch = sd.row_pitch * height;

    GraphicsDevice* device = GetDevice();
    device->CreateTexture(&td, &sd, &imguiFont_);
    io.Fonts->SetTexID((ImTextureID)&imguiFont_);

    SamplerDesc samp;
    samp.address_u = TextureAddressMode::WRAP;
    samp.address_v = TextureAddressMode::WRAP;
    samp.address_w = TextureAddressMode::WRAP;
    samp.filter    = Filter::MIN_MAG_MIP_LINEAR;
    device->CreateSampler(&samp, &imguiSampler_);

    imguiLayout_.elements = {
        { "POSITION", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(ImDrawVert, pos), InputClassification::PER_VERTEX_DATA },
        { "TEXCOORD", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(ImDrawVert, uv),  InputClassification::PER_VERTEX_DATA },
        { "COLOR",    0, Format::R8G8B8A8_UNORM, 0, (uint32_t)offsetof(ImDrawVert, col), InputClassification::PER_VERTEX_DATA },
    };

    // Guard: if the shaders failed to load, CreatePipelineState would dereference a null
    // internal shader state and crash with an access violation. Skip PSO creation instead -
    // ImGui simply won't render, but the app stays alive.
    if (!imguiVS_.IsValid() || !imguiPS_.IsValid()) {
        wi::backlog::post("[ImGui] pipeline not created: shaders invalid. ImGui rendering disabled.",
                          wi::backlog::LogLevel::Error);
        return false;
    }

    PipelineStateDesc psd;
    psd.vs  = &imguiVS_;
    psd.ps  = &imguiPS_;
    psd.il  = &imguiLayout_;
    psd.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHREAD);
    psd.rs  = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
    psd.bs  = wi::renderer::GetBlendState(wi::enums::BSTYPE_TRANSPARENT);
    psd.pt  = PrimitiveTopology::TRIANGLELIST;
    device->CreatePipelineState(&psd, &imguiPSO_);
    return imguiPSO_.IsValid();
}

void DetroitRenderer::RenderImGui (CommandList cmd) {
    // ImGui::Render() was already called in Update() - just retrieve the data.
    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->TotalVtxCount == 0) return;

    // No valid pipeline (shaders missing) -> nothing to draw; binding it would crash.
    if (!imguiPSO_.IsValid()) return;

    const int fb_width  = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int fb_height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0) return;

    GraphicsDevice* device = GetDevice();

    auto vb = device->AllocateGPU(sizeof(ImDrawVert) * drawData->TotalVtxCount, cmd);
    auto ib = device->AllocateGPU(sizeof(ImDrawIdx)  * drawData->TotalIdxCount, cmd);

    auto* vDst = (ImDrawVert*)vb.data;
    auto* iDst = (ImDrawIdx*)ib.data;
    for (int i = 0; i < drawData->CmdListsCount; i++) {
        const ImDrawList* dl = drawData->CmdLists[i];
        std::memcpy(vDst, dl->VtxBuffer.Data, dl->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(iDst, dl->IdxBuffer.Data, dl->IdxBuffer.Size * sizeof(ImDrawIdx));
        vDst += dl->VtxBuffer.Size;
        iDst += dl->IdxBuffer.Size;
    }

    struct ImGuiConstants { float mvp[4][4]; };
    {
        const float L = drawData->DisplayPos.x;
        const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
        const float T = drawData->DisplayPos.y;
        const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
        ImGuiConstants c;
        const float mvp[4][4] = {
            { 2.f/(R-L),    0,          0,    0 },
            { 0,            2.f/(T-B),  0,    0 },
            { 0,            0,          0.5f, 0 },
            { (R+L)/(L-R), (T+B)/(B-T), 0.5f, 1 },
        };
        std::memcpy(&c.mvp, mvp, sizeof(mvp));
        device->BindDynamicConstantBuffer(c, 0, cmd);
    }

    const GPUBuffer* vbs[]     = { &vb.buffer };
    const uint32_t   strides[] = { sizeof(ImDrawVert) };
    const uint64_t   offsets[] = { vb.offset };
    device->BindVertexBuffers(vbs, 0, 1, strides, offsets, cmd);
    device->BindIndexBuffer(&ib.buffer,
        sizeof(ImDrawIdx) == 2 ? IndexBufferFormat::UINT16 : IndexBufferFormat::UINT32,
        ib.offset, cmd);

    Viewport vp;
    vp.width  = (float)fb_width;
    vp.height = (float)fb_height;
    device->BindViewports(1, &vp, cmd);
    device->BindPipelineState(&imguiPSO_, cmd);
    device->BindSampler(&imguiSampler_, 0, cmd);

    const ImVec2 clip_off   = drawData->DisplayPos;
    const ImVec2 clip_scale = drawData->FramebufferScale;
    int global_idx_offset = 0;
    int global_vtx_offset = 0;
    for (int i = 0; i < drawData->CmdListsCount; i++) {
        const ImDrawList* dl = drawData->CmdLists[i];
        for (int ci = 0; ci < dl->CmdBuffer.Size; ci++) {
            const ImDrawCmd& dc = dl->CmdBuffer[ci];
            if (dc.UserCallback) {
                if (dc.UserCallback != ImDrawCallback_ResetRenderState)
                    dc.UserCallback(dl, &dc);
                continue;
            }

            ImVec2 cmin((dc.ClipRect.x - clip_off.x) * clip_scale.x,
                        (dc.ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 cmax((dc.ClipRect.z - clip_off.x) * clip_scale.x,
                        (dc.ClipRect.w - clip_off.y) * clip_scale.y);
            if (cmin.x < 0.f) cmin.x = 0.f;
            if (cmin.y < 0.f) cmin.y = 0.f;
            if (cmax.x > (float)fb_width)  cmax.x = (float)fb_width;
            if (cmax.y > (float)fb_height) cmax.y = (float)fb_height;
            if (cmax.x <= cmin.x || cmax.y <= cmin.y) continue;
            if (dc.ElemCount == 0) continue;

            // Use floor/ceil so a sub-pixel-wide clip never truncates to a degenerate rect
            Rect scissor;
            scissor.left   = (int32_t)std::floor(cmin.x);
            scissor.top    = (int32_t)std::floor(cmin.y);
            scissor.right  = (int32_t)std::ceil(cmax.x);
            scissor.bottom = (int32_t)std::ceil(cmax.y);
            // Guard against degenerate rect after integer conversion
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
            device->BindScissorRects(1, &scissor, cmd);
            const wi::graphics::Texture* tex = (const wi::graphics::Texture*)dc.GetTexID();
            device->BindResource(tex, 0, cmd);
            device->DrawIndexed(dc.ElemCount,
                                (uint32_t)(dc.IdxOffset + global_idx_offset),
                                (int32_t)(dc.VtxOffset  + global_vtx_offset),
                                cmd);
        }
        global_idx_offset += dl->IdxBuffer.Size;
        global_vtx_offset += dl->VtxBuffer.Size;
    }
}

} // namespace st::render::detroit
