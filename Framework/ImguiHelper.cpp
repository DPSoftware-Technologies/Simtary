#include "stApp.h"

st::ImGui_Impl_Data* st::App::ImGui_GetBackendData() {
    return ImGui::GetCurrentContext()
        ? (ImGui_Impl_Data*)ImGui::GetIO().BackendRendererUserData
        : nullptr;
}

ImVec4 RGBA(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void st::App::ImguiInit(SDL_Window *window) {
    // imgui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // Headless (a dedicated server): no window for the platform backend. ImGui still runs -
    // the DevUI code is not special-cased - against a fixed display size, drawn nowhere.
    if (window != nullptr) ImGui_ImplSDL2_InitForVulkan(window);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";  // persist window layout next to the executable
    io.BackendRendererName = "Simtary";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    // Docking: what Editor mode's layout is built on (devui/imeditor.h), and what lets any
    // DevUI panel be dragged into a tab group. libs/ImGui is pinned to the v1.91.9b-docking
    // tag for exactly this - same ImGui version as the master tag, plus the docking branch.
    //
    // Multi-viewport (ImGuiConfigFlags_ViewportsEnable) is deliberately NOT enabled: it
    // needs the RENDERER backend to create a swapchain per platform window, and the backend
    // here is hand-rolled on the engine's graphics device (DetroitRenderer::RenderImGui draws exactly
    // one viewport's draw data into the main swapchain).
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

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
    renderer_->CreateImGuiObjects();   // the font atlas and pipeline, on the device that now exists
}

void st::App::ImguiExit() {
    // Flush the current window layout to imgui.ini before tearing the context down
    // (the periodic auto-save only runs every io.IniSavingRate seconds).
    ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename)
        ImGui::SaveIniSettingsToDisk(io.IniFilename);

    if (window != nullptr) ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void st::App::ImguiUpdate() {
    if (window != nullptr) {
        ImGui_ImplSDL2_NewFrame();
    } else {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(canvas.GetLogicalWidth(), canvas.GetLogicalHeight());
        io.DeltaTime = lastDt_ > 0.0f ? lastDt_ : 1.0f / 60.0f;
    }
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