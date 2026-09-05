#include <string>
#include <fstream>
#include <sstream>

// 1. Core SDL dependencies are required on ALL platforms
#define SDL_MAIN_HANDLED
#include <SDL.h>

// Header-only PNG decoder (no extra linked dependency). Used to load
// crashreportico.png for the window icon + branding image.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>

// 2. Platform-specific API handles
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

static std::string ReadSummary(const std::string& dir) {
#if defined(_WIN32)
    const std::string path = dir + "\\last_crash.txt";
#else
    const std::string path = dir + "/last_crash.txt";
#endif
    std::ifstream f(path);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void OpenFolder(const std::string& dir) {
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + dir + "\"";
    (void)system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + dir + "\"";
    (void)system(cmd.c_str());
#endif
}

// Decode crashreportico.png (next to the reporter executable) into an RGBA SDL
// surface. Returns nullptr if the file is missing or fails to decode. The caller
// owns both the surface (SDL_FreeSurface) and the pixel buffer (returned in
// *outPixels, free with stbi_image_free) - stb owns the pixels, SDL only borrows.
static SDL_Surface* LoadIconSurface(unsigned char** outPixels) {
    *outPixels = nullptr;

    char* base = SDL_GetBasePath(); // directory of this executable, trailing slash
    std::string path = (base ? std::string(base) : std::string()) + "crashreportico.png";
    if (base) SDL_free(base);

    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &n, 4); // force RGBA
    if (!pixels) return nullptr;

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(pixels);
        return nullptr;
    }
    *outPixels = pixels;
    return surf;
}

int main(int argc, char* argv[]) {
    const std::string dir = (argc > 1) ? argv[1] : ".";
    std::string summary = ReadSummary(dir);
    if (summary.empty()) {
        summary = "Simtary crashed, but no crash summary was found.";
    }

    std::string message =
        "Unhandled exception has occurred in engine.\n"
        "************ Exception Text ************\n" + summary;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return 1;

    // Branded window: shows crashreportico.png and carries it as the window/taskbar
    // icon. The OS error dialog below is shown modal on top of it.
    SDL_Window*   iconWindow = nullptr;
    SDL_Renderer* iconRenderer = nullptr;
    unsigned char* iconPixels = nullptr;
    SDL_Surface*  iconSurface = LoadIconSurface(&iconPixels);

    if (iconSurface) {
        // Cap the window to a sensible size while preserving aspect ratio.
        const int maxDim = 256;
        int winW = iconSurface->w, winH = iconSurface->h;
        if (winW > maxDim || winH > maxDim) {
            if (winW >= winH) { winH = winH * maxDim / winW; winW = maxDim; }
            else              { winW = winW * maxDim / winH; winH = maxDim; }
        }

        iconWindow = SDL_CreateWindow("Milistry",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            winW, winH, SDL_WINDOW_SHOWN);

        if (iconWindow) {
            SDL_SetWindowIcon(iconWindow, iconSurface); // titlebar + taskbar

            // SOFTWARE renderer: GPU state is unreliable right after a crash.
            iconRenderer = SDL_CreateRenderer(iconWindow, -1, SDL_RENDERER_SOFTWARE);
            if (iconRenderer) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(iconRenderer, iconSurface);
                SDL_SetRenderDrawColor(iconRenderer, 30, 30, 34, 255);
                SDL_RenderClear(iconRenderer);
                if (tex) SDL_RenderCopy(iconRenderer, tex, nullptr, nullptr);
                SDL_RenderPresent(iconRenderer);
                if (tex) SDL_DestroyTexture(tex);
            }
        }
    }

    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Open Folder" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Ok" },
    };

    SDL_MessageBoxData data = {0};
    data.flags      = SDL_MESSAGEBOX_ERROR;
    data.window     = iconWindow; // modal parent → dialog rides on the branded window
    data.title      = "The engine is crashed";
    data.message    = message.c_str();
    data.numbuttons = SDL_arraysize(buttons);
    data.buttons    = buttons;

    int hit = -1;
    if (SDL_ShowMessageBox(&data, &hit) == 0 && hit == 0) {
        OpenFolder(dir);
    }

    if (iconRenderer) SDL_DestroyRenderer(iconRenderer);
    if (iconWindow)   SDL_DestroyWindow(iconWindow);
    if (iconSurface)  SDL_FreeSurface(iconSurface);
    if (iconPixels)   stbi_image_free(iconPixels);

    SDL_Quit();
    return 0;
}
