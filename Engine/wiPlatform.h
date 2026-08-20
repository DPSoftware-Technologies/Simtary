#pragma once
// This file includes platform, os specific libraries and supplies common platform specific resources

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif // NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SDKDDKVer.h>
#include <windows.h>

#if WINAPI_FAMILY == WINAPI_FAMILY_GAMES
#define PLATFORM_XBOX
#else
#define PLATFORM_WINDOWS_DESKTOP
#endif // WINAPI_FAMILY_GAMES
#define wiLoadLibrary(name) LoadLibraryA(name)
#define wiFreeLibrary(handle) FreeLibrary(handle)
#define wiGetProcAddress(handle,name) GetProcAddress(handle, name)
#elif defined(__SCE__)
#define PLATFORM_PS5
#elif defined(__APPLE__)
#define PLATFORM_APPLE
#if TARGET_OS_OSX
#define PLATFORM_MACOS
#elif TARGET_OS_IOS
#define PLATFORM_IOS
#endif // TARGET_OS_OSX
#include "wiAppleHelper.h"
#include <cstdlib>
#else
#define PLATFORM_LINUX
#endif // _WIN32

#ifdef SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "sdl2.h"
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif
#endif

#if defined(PLATFORM_LINUX) || defined(PLATFORM_APPLE)
#include <dlfcn.h>
#define wiLoadLibrary(name) dlopen(name, RTLD_LAZY)
#define wiFreeLibrary(handle) dlclose(handle)
#define wiGetProcAddress(handle,name) dlsym(handle, name)
typedef void* HMODULE;
#endif // defined(PLATFORM_LINUX) || defined(PLATFORM_APPLE)

namespace wi::platform
{
#ifdef SDL2
	using window_type = SDL_Window*;
	using error_type = int;
#elif defined(_WIN32)
	using window_type = HWND;
	using error_type = HRESULT;
#elif defined(__APPLE__)
	using window_type = void*;
	using error_type = int;
#else
	using window_type = void*;
	using error_type = int;
#endif // SDL2

#if defined(_WIN32)
	inline HWND GetWindowHandle(window_type window)
	{
#ifdef SDL2
		SDL_SysWMinfo wmInfo;
		SDL_VERSION(&wmInfo.version);
		if (SDL_GetWindowWMInfo(window, &wmInfo))
		{
			return wmInfo.info.win.window;
		}
		return nullptr;
#else
		return window;
#endif
	}
#endif

	inline void Exit()
	{
#if defined(SDL2)
		SDL_Event quit_event;
		quit_event.type = SDL_QUIT;
		SDL_PushEvent(&quit_event);
#elif defined(_WIN32)
		PostQuitMessage(0);
#elif defined(__APPLE__)
		std::exit(0);
#endif
		
	}

	struct WindowProperties
	{
		int width = 0;
		int height = 0;
		float dpi = 96;
	};
	inline void GetWindowProperties(window_type window, WindowProperties* dest)
	{
#ifdef SDL2
		int window_width, window_height;
		SDL_GetWindowSize(window, &window_width, &window_height);
		SDL_Vulkan_GetDrawableSize(window, &dest->width, &dest->height);
		dest->dpi = ((float)dest->width / (float)window_width) * 96.f;
#elif defined(PLATFORM_WINDOWS_DESKTOP)
		dest->dpi = (float)GetDpiForWindow(window);
		RECT rect;
		GetClientRect(window, &rect);
		dest->width = int(rect.right - rect.left);
		dest->height = int(rect.bottom - rect.top);
#elif defined(PLATFORM_XBOX)
		dest->dpi = 96.f;
		RECT rect;
		GetClientRect(window, &rect);
		dest->width = int(rect.right - rect.left);
		dest->height = int(rect.bottom - rect.top);
#elif defined(PLATFORM_APPLE)
		XMUINT2 size = wi::apple::GetWindowSize(window);
		dest->width = size.x;
		dest->height = size.y;
		dest->dpi = wi::apple::GetDPIForWindow(window);
#endif
	}

	inline void SetWindowFullScreen(window_type window, bool fullscreen)
	{
#ifdef SDL2
		SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#elif defined(PLATFORM_WINDOWS_DESKTOP)
		// Based on: https://devblogs.microsoft.com/oldnewthing/20100412-00/?p=14353
		static WINDOWPLACEMENT wp = {};
		const DWORD dwStyle = GetWindowLong(window, GWL_STYLE);
		const bool has_overlapped_style = dwStyle & WS_OVERLAPPEDWINDOW;
		const bool isFullScreen = !has_overlapped_style;

		if (fullscreen && !isFullScreen) {
			MONITORINFO mi = { sizeof(mi) };
			if (GetWindowPlacement(window, &wp) &&
				GetMonitorInfo(MonitorFromWindow(window,
					MONITOR_DEFAULTTOPRIMARY), &mi)) {
				SetWindowLong(window, GWL_STYLE,
					dwStyle & ~WS_OVERLAPPEDWINDOW);
				SetWindowPos(window, HWND_TOP,
					mi.rcMonitor.left, mi.rcMonitor.top,
					mi.rcMonitor.right - mi.rcMonitor.left,
					mi.rcMonitor.bottom - mi.rcMonitor.top,
					SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			}
		}
		else if (!fullscreen && isFullScreen) {
			if (!has_overlapped_style) {
				SetWindowLong(window, GWL_STYLE,
					dwStyle | WS_OVERLAPPEDWINDOW);
			}
			SetWindowPlacement(window, &wp);
			SetWindowPos(window, NULL, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
				SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
#elif defined(__APPLE__)
		wi::apple::SetWindowFullScreen(window, fullscreen);
#endif
	}

	inline bool IsWindowFullScreen(window_type window)
	{
#ifdef SDL2
		auto flags = SDL_GetWindowFlags(window);
		if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
			return true;
		else if (flags & SDL_WINDOW_FULLSCREEN)
			return true;
		else
			return false;
#elif defined(PLATFORM_WINDOWS_DESKTOP)
		return (GetWindowLong(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0;
#elif defined(__APPLE__)
		return wi::apple::IsWindowFullScreen(window);
#else
		return true;
#endif
	}
}
