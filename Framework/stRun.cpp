#ifndef SDL_MAIN_HANDLED   // normally set by Simtary::AppFlags
#define SDL_MAIN_HANDLED
#endif
#include "Simtary.h"
#include "sdl2.h"
#include "stRun.h"
#include "crash/CrashHandler.h"
#include "io/UserData.h"
#include "io/asset/AssetSystem.h"
#include "input/InputSystem.h"
#include <cstdio>
#include <cstring>
#include <vector>

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
    // Install the project properties first: the crash reporter, the user-data folder
    // and App::Config() all read from here during startup.
    st::detail::SetActiveConfig(&config);
    st::userdata::Configure(config.organization, config.name);

    // Asset packages, before ANYTHING reads content. wi::helper::FileRead is the single
    // path every engine load goes through, and installing the override here means the
    // splash, the shader warm-up and the first scene all resolve through the packs
    // without one of them having been special-cased. A path no pack holds falls through
    // to the filesystem, so mounting nothing changes nothing.
    for (const std::string& packPath : config.assetPacks) {
        std::string packError;
        if (st::AssetSystem::Get().Mount(packPath, config.assetMountPoint, &packError,
                                         config.assetPacksVerify))
            continue;

        printf("Asset package %s could not be mounted: %s\n", packPath.c_str(), packError.c_str());
        if (config.assetPacksRequired) {
            // A shipped build with a broken install should say so at the door rather
            // than start up and present an empty world.
            wi::helper::messageBox("The game content could not be loaded.\n\n" + packError +
                                   "\n\nReinstalling or verifying the game files should fix this.",
                                   config.name + " - content error");
            return 1;
        }
    }
    if (st::AssetSystem::Get().MountCount() > 0) st::AssetSystem::Get().Install();

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
            // SDL reads the filesystem directly, so it does not see the asset source
            // override the engine uses. A packed-only build would otherwise lose its
            // splash, which is the first thing anyone notices.
            SDL_Surface* splashSurface = SDL_LoadBMP(config.splashImage.c_str()); // BMP = no extra lib needed
            std::vector<uint8_t> splashBytes;
            if (splashSurface == nullptr &&
                st::AssetSystem::Get().Read(config.splashImage, splashBytes) && !splashBytes.empty()) {
                if (SDL_RWops* rw = SDL_RWFromConstMem(splashBytes.data(), (int)splashBytes.size()))
                    splashSurface = SDL_LoadBMP_RW(rw, 1);   // 1 = SDL closes the RWops
            }
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

            // Any genuine input resets the idle standby timer. Done before the
            // ImGui-capture guard below, so typing into a DevUI window still counts
            // as the player being present.
            switch (event.type) {
                case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
                case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
                case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                case SDL_CONTROLLERAXISMOTION:
                case SDL_JOYBUTTONDOWN: case SDL_JOYAXISMOTION:
                case SDL_FINGERDOWN: case SDL_FINGERMOTION:
                    application.Display().NotifyActivity();
                    break;
                default:
                    break;
            }

            // Files dropped onto the window. Handled here rather than left to the generic
            // hook below for two reasons: SDL allocates event.drop.file and the receiver
            // owns it, so the free has to happen on every path; and the capture guard
            // further down only filters pointer and key events, so a drop would sail past
            // it either way. The project gets first refusal through OnEvent, then the
            // DevUI Asset Explorer.
            if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
                if (event.drop.file != nullptr) {
                    if (event.type == SDL_DROPFILE && !application.OnEvent(event))
                        application.HandleDroppedFile(event.drop.file);
                    SDL_free(event.drop.file);
                }
                continue;
            }

            // Developer-tooling toggle. Checked before the capture guard below, not just
            // before the project hook: with an editor panel focused the guard swallows every
            // key, and the one key that brings the tooling back has to survive that. Only a
            // live text field outranks it.
            if (config.devUIToggleKey != 0
                && event.type == SDL_KEYDOWN
                && event.key.repeat == 0
                && !io.WantTextInput
                && event.key.keysym.scancode == (SDL_Scancode)config.devUIToggleKey) {
                application.ToggleDevUI();
                continue;
            }

            // Don't forward pointer/keyboard events to the engine when the UI owns them.
            //
            // Two owners: ImGui's own WantCapture* (the pointer is over a panel, a text field
            // has the keyboard), and st::InputSystem's UI input capture, which Editor mode
            // raises whenever the Game Viewport is not the focused panel. The second exists
            // because a merely focused ImGui window does NOT set WantCaptureKeyboard, so
            // without it WASD typed at the editor viewport still drove the game camera.
            const bool uiOwnsInput = st::InputSystem::Get().IsUIInputCaptured();

            // Releases are never swallowed. Dropping a KEYUP would leave wi::input believing
            // the key is still held once focus moves away mid-press - a camera that flies off
            // forever - so only the press side is filtered.
            const bool isRelease = event.type == SDL_KEYUP
                                || event.type == SDL_MOUSEBUTTONUP;
            const bool isPointer = event.type == SDL_MOUSEMOTION
                                || event.type == SDL_MOUSEBUTTONDOWN
                                || event.type == SDL_MOUSEWHEEL;
            const bool isKey = event.type == SDL_KEYDOWN
                             || event.type == SDL_TEXTINPUT;
            if (!isRelease
                && ((isPointer && (io.WantCaptureMouse || uiOwnsInput))
                 || (isKey && (io.WantCaptureKeyboard || uiOwnsInput))))
                continue;

            // Project hook: raw SDL, before the engine. Returning true consumes it.
            if (application.OnEvent(event))
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
