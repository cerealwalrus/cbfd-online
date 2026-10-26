// (0.10.44) The launcher's own window frame. On Windows the window is made without the Windows title bar and border; the launcher
// draws a title bar of its own along the top (menu_skin.cpp: the star, FIELD LAUNCHER, minimize and close), and SDL's hit test makes
// that bar drag the window (and the window's edges resize it, when it isn't filling the screen). Double-clicking the bar fills the
// desktop's work area (above the taskbar) or goes back to 1600 x 900. When the game starts the Windows border comes back, as before.
//
// CONKER_NATIVE_WINDOW=1 keeps the Windows title bar. Other systems keep theirs.

#include <algorithm>
#include <atomic>
#include <cstdio>

#include <SDL.h>
#include <SDL_syswm.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "conker.hpp"

namespace {
    SDL_Window* chrome_window = nullptr;
    bool active = false;     // the launcher is drawing the title bar
    bool restored = false;   // the game started and the Windows border is back
    bool filled = false;     // the window fills the desktop's work area
    std::atomic<bool> minimize_wanted{ false };
    std::atomic<bool> toggle_wanted{ false };

    // RT64 makes the window full screen itself (Fullscreen in the graphics settings), by changing its Windows style: a popup the
    // monitor's size, with no minimize box. Back to windowed it gives it the Windows border (a caption), which is taken off again.
    enum class WindowStyle { Ours, FullScreen, WithBorder };
#if defined(_WIN32)
    HWND window_handle = nullptr;
#endif
    WindowStyle window_style() {
#if defined(_WIN32)
        if (window_handle != nullptr) {
            const LONG_PTR style = GetWindowLongPtr(window_handle, GWL_STYLE);
            if ((style & WS_CAPTION) == WS_CAPTION) {
                return WindowStyle::WithBorder;
            }
            if ((style & WS_POPUP) != 0 && (style & WS_MINIMIZEBOX) == 0) {
                return WindowStyle::FullScreen;
            }
        }
#endif
        return WindowStyle::Ours;
    }

    bool full_screen(SDL_Window* window) {
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0 || window_style() == WindowStyle::FullScreen;
    }

    // In dp (the UI is 1080 dp high at any window size): the title bar's height, and the width of its buttons at the right
    // (minimize and close, 69 dp each with a little room), which are clicked, not dragged.
    constexpr float ui_height = 1080.0f, bar_height = 51.0f, buttons_width = 150.0f;
    constexpr int edge = 6;   // window units round the edge that resize it

    SDL_HitTestResult SDLCALL hit_test(SDL_Window* window, const SDL_Point* point, void*) {
        if (!active || restored || full_screen(window)) {
            return SDL_HITTEST_NORMAL;
        }
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (!filled && width > 0 && height > 0) {
            const bool left = point->x < edge, right = point->x >= width - edge;
            const bool top = point->y < edge, bottom = point->y >= height - edge;
            if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
            if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
            if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            if (top) return SDL_HITTEST_RESIZE_TOP;
            if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
            if (left) return SDL_HITTEST_RESIZE_LEFT;
            if (right) return SDL_HITTEST_RESIZE_RIGHT;
        }
        const float dp = (float)height / ui_height;
        if ((float)point->y < bar_height * dp && (float)point->x < (float)width - buttons_width * dp) {
            return SDL_HITTEST_DRAGGABLE;
        }
        return SDL_HITTEST_NORMAL;
    }

#if defined(_WIN32)
    // A double click on the bar (Windows calls it the caption): the window has no maximize box, so Windows itself does nothing with it.
    void SDLCALL message_hook(void*, void*, unsigned int message, Uint64 wparam, Sint64) {
        if (message == WM_NCLBUTTONDBLCLK && wparam == HTCAPTION && active && !restored) {
            toggle_wanted = true;
        }
    }
#endif

    SDL_Rect work_area(int display) {
        SDL_Rect area{ 0, 0, 1600, 900 };
        if (display < 0 || SDL_GetDisplayUsableBounds(display, &area) != 0 || area.w <= 0 || area.h <= 0) {
            SDL_GetDisplayBounds(display < 0 ? 0 : display, &area);
        }
        return area;
    }

    void set_filled(bool fill) {
        const SDL_Rect area = work_area(SDL_GetWindowDisplayIndex(chrome_window));
        if (fill) {
            SDL_SetWindowSize(chrome_window, area.w, area.h);
            SDL_SetWindowPosition(chrome_window, area.x, area.y);
        }
        else {
            const int width = std::min(1600, area.w - 80), height = std::min(900, area.h - 60);
            SDL_SetWindowSize(chrome_window, width, height);
            SDL_SetWindowPosition(chrome_window, area.x + (area.w - width) / 2, area.y + (area.h - height) / 2);
        }
        filled = fill;
    }
}

bool conker::window_chrome::wanted() {
#if defined(_WIN32)
    const char* native = SDL_getenv("CONKER_NATIVE_WINDOW");
    return native == nullptr || *native == 0 || *native == '0';
#else
    return false;
#endif
}

bool conker::window_chrome::start_rect(bool fill, int* x, int* y, int* width, int* height) {
    if (!fill) {
        return false;
    }
    const SDL_Rect area = work_area(0);
    *x = area.x;
    *y = area.y;
    *width = area.w;
    *height = area.h;
    return true;
}

void conker::window_chrome::attach(SDL_Window* window, bool is_filled) {
    chrome_window = window;
    filled = is_filled;
#if defined(_WIN32)
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info)) {
        window_handle = info.info.win.window;
    }
#endif
    if (SDL_SetWindowHitTest(window, hit_test, nullptr) != 0) {
        std::printf("[window] no hit test (%s): the Windows title bar stays\n", SDL_GetError());
        SDL_SetWindowBordered(window, SDL_TRUE);
        return;
    }
#if defined(_WIN32)
    SDL_SetWindowsMessageHook(message_hook, nullptr);
#endif
    active = true;
    std::printf("[window] the launcher's own title bar (%s)\n", filled ? "filling the work area" : "windowed");
}

bool conker::window_chrome::custom() {
    return active && !restored;
}

void conker::window_chrome::minimize() {
    minimize_wanted = true;
}

void conker::window_chrome::toggle_fill() {
    toggle_wanted = true;
}

void conker::window_chrome::update(bool game_started) {
    if (!active || restored || chrome_window == nullptr) {
        return;
    }
    const Uint32 flags = SDL_GetWindowFlags(chrome_window);
    if (game_started) {
        // the Windows border back for the game
        restored = true;
        SDL_SetWindowHitTest(chrome_window, nullptr, nullptr);
#if defined(_WIN32)
        SDL_SetWindowsMessageHook(nullptr, nullptr);
#endif
        if (full_screen(chrome_window)) {
            // full screen: RT64 gives it the Windows border when it goes back to windowed
            std::printf("[window] the game started full screen\n");
            return;
        }
        SDL_SetWindowBordered(chrome_window, SDL_TRUE);
        if (filled) {
            SDL_MaximizeWindow(chrome_window);
        }
        else {
            // the title bar is added above the window: keep it on the screen
            int top = 0, x = 0, y = 0;
            SDL_GetWindowBordersSize(chrome_window, &top, nullptr, nullptr, nullptr);
            SDL_GetWindowPosition(chrome_window, &x, &y);
            const SDL_Rect area = work_area(SDL_GetWindowDisplayIndex(chrome_window));
            if (y - top < area.y) {
                SDL_SetWindowPosition(chrome_window, x, area.y + top);
            }
        }
        std::printf("[window] the game started: the Windows title bar is back\n");
        return;
    }
    if (full_screen(chrome_window)) {
        return;
    }
    if (window_style() == WindowStyle::WithBorder) {
        // RT64 went back to windowed with the Windows border: off again (SDL still has it as borderless, so it's put on and taken off),
        // at the size it had
        SDL_SetWindowBordered(chrome_window, SDL_TRUE);
        SDL_SetWindowBordered(chrome_window, SDL_FALSE);
        set_filled(filled);
        std::printf("[window] out of full screen: the launcher's own title bar again\n");
    }
    if ((SDL_GetWindowFlags(chrome_window) & SDL_WINDOW_MAXIMIZED) != 0) {
        // maximized some other way (Windows+Up): a window with no border would cover the taskbar, so it fills the work area instead
        SDL_RestoreWindow(chrome_window);
        set_filled(true);
    }
    if (minimize_wanted.exchange(false)) {
        SDL_MinimizeWindow(chrome_window);
    }
    if (toggle_wanted.exchange(false) && (flags & SDL_WINDOW_MINIMIZED) == 0) {
        set_filled(!filled);
    }
}
