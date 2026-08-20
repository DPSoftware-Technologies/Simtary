#ifndef SDL_MAIN_HANDLED   // normally set by Simtary::AppFlags
#define SDL_MAIN_HANDLED
#endif
#include "Simtary.h"
#include "sdl2.h"
#include "stRun.h"
#include "crash/CrashHandler.h"
#include "io/UserData.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <SDL_syswm.h>
#include <dwmapi.h>

// Define fallbacks if compiling with an older Windows SDK
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

void DisableWindowRounding(SDL_Window* sdlWindow) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);

    if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
        if (wmInfo.subsystem == SDL_SYSWM_WINDOWS) {
            HWND hwnd = wmInfo.info.win.window;
            DWORD preference = DWMWCP_DONOTROUND;

            DwmSetWindowAttribute(
                hwnd, 
                (DWMWINDOWATTRIBUTE)DWMWA_WINDOW_CORNER_PREFERENCE, 
                &preference, 
                sizeof(preference)
            );
        }
    }
}
#else
void DisableWindowRounding(SDL_Window* sdlWindow) {
    // No-op for macOS, Linux, etc.
}
#endif

namespace st::detail { void SetActiveConfig(const AppConfig* config); }

int st::Run(int argc, char* argv[], AppConfig& config, App& application) {
    // Install the project properties first: the crash reporter, the user-data
    // folder and App::Config() all read from here during startup.
    st::detail::SetActiveConfig(&config);
    st::userdata::Configure(config.organization, config.name);

    printf("Starting crash reporter.\n");
    st::crash::Init(argv[0], config.name.c_str());

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--testcrash") == 0 || strcmp(argv[i], "testcrash") == 0) {
            volatile int* p = nullptr;
            *p = 42;
        }
    }

    printf("Starting chassis\n");
    sdl2::sdlsystem_ptr_t system = sdl2::make_sdlsystem(SDL_INIT_EVERYTHING | SDL_INIT_EVENTS);
    sdl2::window_ptr_t window = sdl2::make_window(
            config.name.c_str(),
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            config.windowWidth, config.windowHeight,
            SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window) {
        DisableWindowRounding(window.get());
    }
    // Splash screen. Optional: a project with no splashImage (or a missing file)
    // just gets a blank window until the graphics device comes up.
    if (!config.splashImage.empty()) {
        // SOFTWARE renderer — Vulkan ain't active yet, use this!
        SDL_Renderer* splashRenderer = SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE);
        if (splashRenderer) {
            SDL_Surface* splashSurface = SDL_LoadBMP(config.splashImage.c_str()); // BMP = no extra lib needed
            SDL_Texture* splashTexture = splashSurface
                ? SDL_CreateTextureFromSurface(splashRenderer, splashSurface)
                : nullptr;
            if (splashSurface) SDL_FreeSurface(splashSurface);

            if (splashTexture) {
                int winW, winH, imgW, imgH;
                SDL_GetWindowSize(window.get(), &winW, &winH);
                SDL_QueryTexture(splashTexture, NULL, NULL, &imgW, &imgH);

                // CENTER MATH — NO EXCUSES!
                SDL_Rect dst = {
                    (winW - imgW) / 2,
                    (winH - imgH) / 2,
                    imgW, imgH
                };

                SDL_RenderClear(splashRenderer);
                SDL_RenderCopy(splashRenderer, splashTexture, NULL, &dst);
                SDL_RenderPresent(splashRenderer);

                // Cleanup splash before Vulkan takes over
                SDL_DestroyTexture(splashTexture);
            }
            SDL_DestroyRenderer(splashRenderer);
        }
    }

    printf("Starting Engine.\n");
    application.SetWindow(window.get());
    printf("Starting Engine..\n");
    wi::arguments::Parse(argc, argv);

    // application.infoDisplay.active = true;
    // application.infoDisplay.watermark = true;
    // application.infoDisplay.resolution = true;
    // application.infoDisplay.fpsinfo = true;

    printf("Starting Engine...\n");
    application.Initialize();

    bool quit = false;
    printf("Starting Engine....\n");
    while (!quit) {
        SDL_PumpEvents();
        application.Run();

        SDL_Event event;

        SDL_SetWindowTitle(window.get(), ((config.name + " - ") + application.infowatermark_str).c_str());
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            const ImGuiIO& io = ImGui::GetIO();

            switch (event.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_WINDOWEVENT:
                    switch (event.window.event) {
                        case SDL_WINDOWEVENT_CLOSE:
                            quit = true;
                            break;
                        case SDL_WINDOWEVENT_RESIZED:
                            application.SetWindow(application.window);
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }

            // Don't forward pointer/keyboard events to the engine when ImGui owns them
            const bool isPointer = event.type == SDL_MOUSEMOTION
                                || event.type == SDL_MOUSEBUTTONDOWN
                                || event.type == SDL_MOUSEBUTTONUP
                                || event.type == SDL_MOUSEWHEEL;
            const bool isKey = event.type == SDL_KEYDOWN
                             || event.type == SDL_KEYUP
                             || event.type == SDL_TEXTINPUT;
            if ((isPointer && io.WantCaptureMouse) || (isKey && io.WantCaptureKeyboard))
                continue;
            wi::input::sdlinput::ProcessEvent(event);
        }
    }
    printf("Stopping Content\n");
    application.Exit();
    printf("Stopping Engine\n");
    wi::jobsystem::ShutDown();
    printf("Stopping chassis\n");
    SDL_Quit();
    printf("Stopping crash reporter \n");
    st::crash::Shutdown();
    printf("Stopped\n");
    return 0;
}
