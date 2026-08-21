#include "stApp.h"

st::ImGui_Impl_Data* st::App::ImGui_GetBackendData() {
    return ImGui::GetCurrentContext()
        ? (ImGui_Impl_Data*)ImGui::GetIO().BackendRendererUserData
        : nullptr;
}

void st::App::ImGui_CreateDeviceObjects() {
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

    wi::graphics::GetDevice()->CreateTexture(&td, &sd, &fontTexture);

    io.Fonts->SetTexID((ImTextureID)&fontTexture);

    SamplerDesc samp;
    samp.address_u = TextureAddressMode::WRAP;
    samp.address_v = TextureAddressMode::WRAP;
    samp.address_w = TextureAddressMode::WRAP;
    samp.filter    = Filter::MIN_MAG_MIP_LINEAR;
    wi::graphics::GetDevice()->CreateSampler(&samp, &sampler);

    imguiInputLayout.elements = {
        { "POSITION", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(ImDrawVert, pos), InputClassification::PER_VERTEX_DATA },
        { "TEXCOORD", 0, Format::R32G32_FLOAT,   0, (uint32_t)offsetof(ImDrawVert, uv),  InputClassification::PER_VERTEX_DATA },
        { "COLOR",    0, Format::R8G8B8A8_UNORM, 0, (uint32_t)offsetof(ImDrawVert, col), InputClassification::PER_VERTEX_DATA },
    };

    PipelineStateDesc psd;
    psd.vs  = &imguiVS;
    psd.ps  = &imguiPS;
    psd.il  = &imguiInputLayout;
    psd.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHREAD);
    psd.rs  = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
    psd.bs  = wi::renderer::GetBlendState(wi::enums::BSTYPE_TRANSPARENT);
    psd.pt  = PrimitiveTopology::TRIANGLELIST;

    // Guard: if the shaders failed to load (e.g. shaders/ImGuiVS.cso missing next to
    // the executable), CreatePipelineState would dereference a null internal shader
    // state and crash with an access violation. Skip PSO creation instead — ImGui
    // simply won't render, but the app stays alive.
    if (!imguiVS.IsValid() || !imguiPS.IsValid()) {
        wi::backlog::post(
            "[ImGui] pipeline not created: shaders invalid. ImGui rendering disabled.",
            wi::backlog::LogLevel::Error);
        return;
    }
    wi::graphics::GetDevice()->CreatePipelineState(&psd, &imguiPSO);
}

ImVec4 RGBA(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void st::App::ImguiInit(SDL_Window *window) {
    // Load the precompiled ImGui shaders. LoadShader reads from
    // wi::renderer::GetShaderPath(), which by this point already has the backend
    // subfolder appended (Application::SetWindow ran in main before Initialize):
    // "<exe>/shaders/hlsl6/" on DX12, "<exe>/shaders/spirv/" on Vulkan. The build's
    // post-build step drops ImGuiVS.cso / ImGuiPS.cso into those folders.
    bool vsOK = wi::renderer::LoadShader(ShaderStage::VS, imguiVS, "ImGuiVS.cso");
    bool psOK = wi::renderer::LoadShader(ShaderStage::PS, imguiPS, "ImGuiPS.cso");
    if (!vsOK || !psOK) {
        wi::backlog::post(
            "[ImGui] failed to load ImGuiVS.cso / ImGuiPS.cso from " +
            wi::renderer::GetShaderPath() +
            ". Build with dxc available, or copy the precompiled .cso files there. "
            "ImGui rendering will be disabled.",
            wi::backlog::LogLevel::Error);
    }

    // imgui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForVulkan(window);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";  // persist window layout next to the executable
    io.BackendRendererName = "Simtary";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    // theme
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text]                 = RGBA(220, 230, 220);
    colors[ImGuiCol_TextDisabled]         = RGBA(110, 120, 110);

    colors[ImGuiCol_WindowBg]             = RGBA(22, 25, 22);
    colors[ImGuiCol_ChildBg]              = RGBA(18, 21, 18);
    colors[ImGuiCol_PopupBg]              = RGBA(28, 32, 28);

    colors[ImGuiCol_Border]               = RGBA(70, 80, 70);
    colors[ImGuiCol_BorderShadow]         = RGBA(0, 0, 0, 0);

    // Frames
    colors[ImGuiCol_FrameBg]              = RGBA(38, 45, 38);
    colors[ImGuiCol_FrameBgHovered]       = RGBA(55, 70, 55);
    colors[ImGuiCol_FrameBgActive]        = RGBA(70, 90, 70);

    // Title bars
    colors[ImGuiCol_TitleBg]              = RGBA(25, 35, 25);
    colors[ImGuiCol_TitleBgActive]        = RGBA(45, 70, 45);
    colors[ImGuiCol_TitleBgCollapsed]     = RGBA(20, 25, 20);

    // Menu
    colors[ImGuiCol_MenuBarBg]            = RGBA(28, 35, 28);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = RGBA(18, 22, 18);
    colors[ImGuiCol_ScrollbarGrab]        = RGBA(65, 80, 65);
    colors[ImGuiCol_ScrollbarGrabHovered] = RGBA(90, 110, 90);
    colors[ImGuiCol_ScrollbarGrabActive]  = RGBA(110, 130, 110);

    // Checkmark
    colors[ImGuiCol_CheckMark]            = RGBA(120, 220, 120);

    // Sliders
    colors[ImGuiCol_SliderGrab]           = RGBA(90, 160, 90);
    colors[ImGuiCol_SliderGrabActive]     = RGBA(130, 220, 130);

    // Buttons
    colors[ImGuiCol_Button]               = RGBA(55, 70, 55);
    colors[ImGuiCol_ButtonHovered]        = RGBA(75, 100, 75);
    colors[ImGuiCol_ButtonActive]         = RGBA(100, 135, 100);

    // Headers
    colors[ImGuiCol_Header]               = RGBA(45, 60, 45);
    colors[ImGuiCol_HeaderHovered]        = RGBA(70, 95, 70);
    colors[ImGuiCol_HeaderActive]         = RGBA(95, 125, 95);

    // Tabs
    colors[ImGuiCol_Tab]                  = RGBA(35, 45, 35);
    colors[ImGuiCol_TabHovered]           = RGBA(70, 100, 70);
    colors[ImGuiCol_TabActive]            = RGBA(55, 85, 55);
    colors[ImGuiCol_TabUnfocused]         = RGBA(28, 35, 28);
    colors[ImGuiCol_TabUnfocusedActive]   = RGBA(40, 55, 40);

    // Resize Grip
    colors[ImGuiCol_ResizeGrip]           = RGBA(70, 90, 70);
    colors[ImGuiCol_ResizeGripHovered]    = RGBA(100, 130, 100);
    colors[ImGuiCol_ResizeGripActive]     = RGBA(130, 170, 130);

    // Plots
    colors[ImGuiCol_PlotLines]            = RGBA(120, 220, 120);
    colors[ImGuiCol_PlotLinesHovered]     = RGBA(180, 255, 180);
    colors[ImGuiCol_PlotHistogram]        = RGBA(170, 200, 100);
    colors[ImGuiCol_PlotHistogramHovered] = RGBA(220, 240, 120);

    // Selection
    colors[ImGuiCol_TextSelectedBg]       = RGBA(70, 120, 70, 180);

    // Drag & Drop
    colors[ImGuiCol_DragDropTarget]       = RGBA(255, 255, 0);

    // Navigation
    colors[ImGuiCol_NavHighlight]         = RGBA(120, 220, 120);
    colors[ImGuiCol_NavWindowingHighlight]= RGBA(255, 255, 255);

    ImGui_Impl_Data* bd = IM_NEW(ImGui_Impl_Data)();
    io.BackendRendererUserData = (void*)bd;

    wi::Application::Initialize();
    ImGui_CreateDeviceObjects();
}

void st::App::ImguiCompose(wi::graphics::CommandList cmd) {
    // ImGui::Render() was already called in Update() — just retrieve the data.
    auto* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->TotalVtxCount == 0) return;

    // No valid pipeline (shaders missing) → nothing to draw; binding it would crash.
    if (!imguiPSO.IsValid()) return;

    int fb_width  = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fb_height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0) return;

    GraphicsDevice* device = wi::graphics::GetDevice();

    auto vb = device->AllocateGPU(sizeof(ImDrawVert) * drawData->TotalVtxCount, cmd);
    auto ib = device->AllocateGPU(sizeof(ImDrawIdx)  * drawData->TotalIdxCount, cmd);

    auto* vDst = (ImDrawVert*)vb.data;
    auto* iDst = (ImDrawIdx*)ib.data;
    for (int i = 0; i < drawData->CmdListsCount; i++) {
        const ImDrawList* dl = drawData->CmdLists[i];
        memcpy(vDst, dl->VtxBuffer.Data, dl->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(iDst, dl->IdxBuffer.Data, dl->IdxBuffer.Size * sizeof(ImDrawIdx));
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
        float mvp[4][4] = {
            { 2.f/(R-L),    0,          0,    0 },
            { 0,            2.f/(T-B),  0,    0 },
            { 0,            0,          0.5f, 0 },
            { (R+L)/(L-R), (T+B)/(B-T), 0.5f, 1 },
        };
        memcpy(&c.mvp, mvp, sizeof(mvp));
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
    device->BindPipelineState(&imguiPSO, cmd);
    device->BindSampler(&sampler, 0, cmd);

    ImVec2 clip_off   = drawData->DisplayPos;
    ImVec2 clip_scale = drawData->FramebufferScale;
    int global_idx_offset = 0;
    int global_vtx_offset = 0;
    for (int i = 0; i < drawData->CmdListsCount; i++) {
        const ImDrawList* dl = drawData->CmdLists[i];
        for (int ci = 0; ci < dl->CmdBuffer.Size; ci++) {
            const ImDrawCmd& cmd_ = dl->CmdBuffer[ci];
            if (cmd_.UserCallback) {
                if (cmd_.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd_.UserCallback(dl, &cmd_);
                continue;
            }

            ImVec2 cmin((cmd_.ClipRect.x - clip_off.x) * clip_scale.x,
                        (cmd_.ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 cmax((cmd_.ClipRect.z - clip_off.x) * clip_scale.x,
                        (cmd_.ClipRect.w - clip_off.y) * clip_scale.y);
            if (cmin.x < 0.f) cmin.x = 0.f;
            if (cmin.y < 0.f) cmin.y = 0.f;
            if (cmax.x > (float)fb_width)  cmax.x = (float)fb_width;
            if (cmax.y > (float)fb_height) cmax.y = (float)fb_height;
            if (cmax.x <= cmin.x || cmax.y <= cmin.y) continue;
            if (cmd_.ElemCount == 0) continue;

            // Use floor/ceil so a sub-pixel-wide clip never truncates to a degenerate rect
            Rect scissor;
            scissor.left   = (int32_t)std::floor(cmin.x);
            scissor.top    = (int32_t)std::floor(cmin.y);
            scissor.right  = (int32_t)std::ceil(cmax.x);
            scissor.bottom = (int32_t)std::ceil(cmax.y);
            // Guard against degenerate rect after integer conversion
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
                ci, cmd_.ElemCount,
                (uint32_t)(cmd_.IdxOffset + global_idx_offset),
                (int32_t)(cmd_.VtxOffset  + global_vtx_offset),
                scissor.left, scissor.top, scissor.right, scissor.bottom,
                (void*)cmd_.GetTexID();
            device->BindScissorRects(1, &scissor, cmd);
            const Texture* tex = (const Texture*)cmd_.GetTexID();
            device->BindResource(tex, 0, cmd);
            device->DrawIndexed(cmd_.ElemCount,
                                (uint32_t)(cmd_.IdxOffset + global_idx_offset),
                                (int32_t)(cmd_.VtxOffset  + global_vtx_offset),
                                cmd);
        }
        global_idx_offset += dl->IdxBuffer.Size;
        global_vtx_offset += dl->VtxBuffer.Size;
    }
}

void st::App::ImguiExit() {
    // Flush the current window layout to imgui.ini before tearing the context down
    // (the periodic auto-save only runs every io.IniSavingRate seconds).
    ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename)
        ImGui::SaveIniSettingsToDisk(io.IniFilename);

    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void st::App::ImguiUpdate() {
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Developer tooling first, so game UI and the loading screen draw over it.
    if (IsDevUIVisible()) DevUIRender();

    // The active scene: game-facing UI always, dev-only panels only with DevUI up.
    sceneManager.OnGUI();
    if (IsDevUIVisible()) sceneManager.OnDevGUI();

    // Project hook: the game's own UI. Drawn regardless of DevUIMode.
    RenderUI();

    // On top of everything, including while shaders are still compiling.
    RenderLoadingScreen(loading_);

    ImGui::Render();
}