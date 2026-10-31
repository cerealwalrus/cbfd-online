// (0.10.44) The launcher's own window frame: on Windows the launcher draws its title bar (menu_skin.cpp: the star, FIELD LAUNCHER,
// minimize, maximize / restore and close) in place of the Windows one. When the game starts the Windows title bar comes back.
//
// (0.10.50) The window is a normal Windows window underneath (caption, sizing frame, minimize and maximize boxes), so Windows treats it
// as one: dragging it to a screen edge snaps it, the Snap Layouts flyout opens over its maximize button (Windows 11), Windows+arrows
// work, its edges resize it, and maximized it stops at the taskbar. Only the frame isn't drawn: SDL's window procedure is wrapped, and
// WM_NCCALCSIZE gives the whole window to the picture, WM_NCHITTEST says where the title bar, the maximize button and the edges are.
// (0.10.44 to 0.10.49 made it a borderless popup, which Windows doesn't snap or lay out.)
//
// CONKER_NATIVE_WINDOW=1 keeps the Windows title bar. Other systems keep theirs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>
#include <SDL_syswm.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

#include "conker.hpp"

namespace {
    SDL_Window* chrome_window = nullptr;
    bool active = false;                    // the launcher draws the title bar (until the game starts)
    std::atomic<bool> restored{ false };    // the game started and the Windows title bar is back
    std::atomic<bool> zoomed{ false };      // maximized (read by the launcher's middle button)
    std::atomic<bool> max_hovered{ false }; // the mouse is on the maximize button (Windows has it: it's where Snap Layouts opens)
    std::atomic<bool> minimize_wanted{ false };
    std::atomic<bool> toggle_wanted{ false };
    std::atomic<bool> config_full_screen{ false };

    // In dp (the UI is 1080 dp high at any window size): the title bar's height, and its buttons at the right: close, maximize /
    // restore and minimize, 69 dp each, 3 apart (menu_skin.cpp).
    constexpr float ui_height = 1080.0f, bar_height = 51.0f, button_width = 69.0f, button_gap = 3.0f;

#if defined(_WIN32)
    HWND window_handle = nullptr;
    WNDPROC sdl_procedure = nullptr;
    bool max_pressed = false;

    UINT window_dpi(HWND hwnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static GetDpiForWindowFn get = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        const UINT dpi = get != nullptr ? get(hwnd) : 96;
        return dpi != 0 ? dpi : 96;
    }

    // the launcher's frame applies: not once the game started, nor in full screen (RT64's: a popup with no caption)
    bool own_frame(HWND hwnd) {
        return active && !restored.load() && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
    }

    LRESULT hit_test(HWND hwnd, LPARAM lparam) {
        POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right, height = client.bottom;
        if (!IsZoomed(hwnd)) {
            const int edge = MulDiv(6, (int)window_dpi(hwnd), 96);
            const bool left = point.x < edge, right = point.x >= width - edge;
            const bool top = point.y < edge, bottom = point.y >= height - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
        }
        const float dp = (float)height / ui_height;
        if (dp > 0.0f && (float)point.y < bar_height * dp) {
            const float from_right = (float)(width - point.x) / dp;   // dp from the window's right edge
            const float max_left = 2.0f * button_width + button_gap, max_right = button_width + button_gap;
            if (from_right >= max_right && from_right < max_left) {
                return HTMAXBUTTON;
            }
            if (from_right < 3.0f * button_width + 2.0f * button_gap + 4.0f) {
                return HTCLIENT;   // close and minimize: the launcher's own buttons
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    LRESULT CALLBACK chrome_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (own_frame(hwnd)) {
            switch (message) {
            case WM_NCCALCSIZE:
                if (wparam == TRUE) {
                    // no frame: the whole window is the picture; maximized, Windows makes the window its frame's width bigger than
                    // the work area all round, so the picture is the work area
                    NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lparam;
                    if (IsZoomed(hwnd)) {
                        MONITORINFO monitor{};
                        monitor.cbSize = sizeof(monitor);
                        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                            params->rgrc[0] = monitor.rcWork;
                        }
                    }
                }
                return 0;
            case WM_NCHITTEST:
                return hit_test(hwnd, lparam);
            case WM_NCACTIVATE:
                // (no frame to repaint as the window gains or loses focus)
                return DefWindowProcW(hwnd, message, wparam, -1);
            case WM_NCMOUSEMOVE:
                if (wparam == HTMAXBUTTON) {
                    if (!max_hovered.exchange(true)) {
                        TRACKMOUSEEVENT track{};
                        track.cbSize = sizeof(track);
                        track.dwFlags = TME_LEAVE | TME_NONCLIENT;
                        track.hwndTrack = hwnd;
                        TrackMouseEvent(&track);
                    }
                }
                else {
                    max_hovered = false;
                }
                break;
            case WM_NCMOUSELEAVE:
            case WM_MOUSEMOVE:
                max_hovered = false;
                break;
            case WM_NCLBUTTONDOWN:
                if (wparam == HTMAXBUTTON) {
                    max_pressed = true;
                    return 0;   // (Windows would draw a button of its own)
                }
                max_pressed = false;
                break;
            case WM_NCLBUTTONUP:
                if (wparam == HTMAXBUTTON) {
                    if (max_pressed) {
                        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    }
                    max_pressed = false;
                    return 0;
                }
                max_pressed = false;
                break;
            case WM_NCLBUTTONDBLCLK:
                if (wparam == HTMAXBUTTON) {
                    return 0;
                }
                break;   // (on the title bar: Windows maximizes or restores)
            default:
                break;
            }
        }
        return CallWindowProcW(sdl_procedure, hwnd, message, wparam, lparam);
    }

    RECT work_area(HWND hwnd) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor);
        return monitor.rcWork;
    }

    // the window 1600 x 900 (smaller on a smaller screen) in the middle of its screen's work area, not maximized
    void centre_1600x900(HWND hwnd) {
        if (IsZoomed(hwnd) || IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        const RECT area = work_area(hwnd);
        const int area_width = area.right - area.left, area_height = area.bottom - area.top;
        const int width = (std::min)(1600, area_width - 80), height = (std::min)(900, area_height - 60);
        SetWindowPos(hwnd, nullptr, area.left + (area_width - width) / 2, area.top + (area_height - height) / 2, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void minimize_now() {
        ReleaseCapture();
        ShowWindow(window_handle, SW_MINIMIZE);
        std::printf("[window] minimizing: %s\n", IsIconic(window_handle) ? "done" : "Windows didn't");
    }
#endif
}

bool conker::window_chrome::wanted() {
#if defined(_WIN32)
    const char* native = SDL_getenv("CONKER_NATIVE_WINDOW");
    return native == nullptr || *native == 0 || *native == '0';
#else
    return false;
#endif
}

// (possess75) CONKER_WINDOW_TILE: the quarter of the screen this window opens in (1 to 4), 0 if none (frontend.cpp's create_window)
int conker::window_chrome::tile() {
    static const int value = []() {
        const char* text = std::getenv("CONKER_WINDOW_TILE");
        const int n = text != nullptr ? std::atoi(text) : 0;
        return (n >= 1 && n <= 4) ? n : 0;
    }();
    return value;
}

// (possess76) CONKER_WINDOW_DISPLAY: the display the window opens on, as SDL counts them (0 if not set), found from the number Windows'
// Display settings give it (1 on: the display's DISPLAY<n> device name); the displays are listed in conker_displays.txt
int conker::window_chrome::display_index() {
    static const int value = []() {
        const int count = SDL_GetNumVideoDisplays();
        FILE* list = std::fopen("conker_displays.txt", "w");
        const char* text = std::getenv("CONKER_WINDOW_DISPLAY");
        const int wanted = text != nullptr ? std::atoi(text) : 0;
        int found = 0;
        for (int i = 0; i < count; i++) {
            SDL_Rect bounds{};
            SDL_GetDisplayBounds(i, &bounds);
            int number = i + 1;
#if defined(_WIN32)
            // the display's number in Windows' settings, from its device name (\\.\DISPLAY3 is display 3)
            POINT centre{ bounds.x + bounds.w / 2, bounds.y + bounds.h / 2 };
            HMONITOR monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXA info{};
            info.cbSize = sizeof(info);
            if (monitor != nullptr && GetMonitorInfoA(monitor, &info)) {
                const char* digits = info.szDevice;
                while (*digits != 0 && (*digits < '0' || *digits > '9')) {
                    digits++;
                }
                if (*digits != 0) {
                    number = std::atoi(digits);
                }
            }
#endif
            const char* name = SDL_GetDisplayName(i);
            if (list != nullptr) {
                std::fprintf(list, "display %d (SDL's %d): %s, %d x %d at %d, %d\n", number, i, name != nullptr ? name : "?", bounds.w, bounds.h,
                    bounds.x, bounds.y);
            }
            if (wanted > 0 && number == wanted) {
                found = i;
            }
        }
        if (list != nullptr) {
            std::fprintf(list, "CONKER_WINDOW_DISPLAY=%d: SDL's display %d\n", wanted, found);
            std::fclose(list);
        }
        return found;
    }();
    return value;
}

bool conker::window_chrome::start_rect(bool, int* x, int* y, int* width, int* height) {
    // the window's place when it isn't maximized: its size (1600 x 900, or --window's), smaller on a smaller screen, in the middle
    SDL_Rect area{ 0, 0, 1600, 900 };
    if (SDL_GetDisplayUsableBounds(0, &area) != 0 || area.w <= 0 || area.h <= 0) {
        SDL_GetDisplayBounds(0, &area);
    }
    *width = (std::min)(*width, area.w - 80);
    *height = (std::min)(*height, area.h - 60);
    *x = area.x + (area.w - *width) / 2;
    *y = area.y + (area.h - *height) / 2;
    return false;
}

void conker::window_chrome::attach(SDL_Window* window, bool maximized, int x, int y, int width, int height) {
    chrome_window = window;
#if defined(_WIN32)
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.info.win.window == nullptr) {
        SDL_ShowWindow(window);
        if (maximized) {
            SDL_MaximizeWindow(window);
        }
        return;
    }
    window_handle = info.info.win.window;
    sdl_procedure = (WNDPROC)SetWindowLongPtrW(window_handle, GWLP_WNDPROC, (LONG_PTR)chrome_procedure);
    active = sdl_procedure != nullptr;
    if (!active) {
        std::printf("[window] couldn't take over the frame: the Windows title bar stays\n");
    }
    // the frame recomputed (none), the window's place set (its whole size now the picture), then shown
    // (0.11.4) not maximized yet (frontend.cpp's create_window makes it so): its place first, then maximized, so Windows sizes it to the
    // screen and keeps 1600 x 900 as its place when it's restored
    if (IsZoomed(window_handle)) {
        ShowWindow(window_handle, SW_RESTORE);
    }
    SetWindowPos(window_handle, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(window_handle, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetForegroundWindow(window_handle);
    zoomed = IsZoomed(window_handle) != 0;
    std::printf("[window] the launcher's own title bar (%s), style 0x%llx\n", zoomed.load() ? "maximized" : "windowed",
        (unsigned long long)GetWindowLongPtrW(window_handle, GWL_STYLE));
#else
    (void)x; (void)y; (void)width; (void)height;
    SDL_ShowWindow(window);
    if (maximized) {
        SDL_MaximizeWindow(window);
    }
#endif
}

bool conker::window_chrome::custom() {
    return active && !restored.load();
}

void conker::window_chrome::hide_for_quit() {
#if defined(_WIN32)
    if (window_handle != nullptr) {
        ShowWindowAsync(window_handle, SW_HIDE);   // (async: from any thread, without waiting on the window's own)
    }
#endif
}

void conker::window_chrome::minimize() {
    std::printf("[window] minimize pressed\n");
    minimize_wanted = true;
}

bool conker::window_chrome::is_filled() {
    return zoomed.load();
}

bool conker::window_chrome::maximize_hovered() {
    return max_hovered.load();
}

void conker::window_chrome::toggle_fill() {
    std::printf("[window] maximize / restore pressed\n");
    toggle_wanted = true;
}

void conker::window_chrome::update(bool game_started, bool settings_full_screen) {
    config_full_screen = settings_full_screen;
#if defined(_WIN32)
    if (!active || window_handle == nullptr) {
        return;
    }
    HWND hwnd = window_handle;
    zoomed = IsZoomed(hwnd) != 0;
    {
        // (0.11.4) Opened as a 1600 x 900 window, the launcher's buttons and the mouse disagreed (the button lit was one or two above
        // the cursor, more so further from the window's middle) until F11 was pressed twice: the window maximized and put back to
        // 1600 x 900. Neither a 2-pixel resize nor a maximize and back as it opened did (the window, SDL and the picture all agree on
        // its size). The sizes, and where the mouse is by Windows and by SDL, are written to launcher_window.log for the first 10
        // seconds; 
        static int frames_up = 0;
        static bool cycled = false;
        static FILE* log_file = nullptr;
        static auto opened_at = std::chrono::steady_clock::now();
        static int last_logged_ms = -1000;
        const auto log_sizes = [&](const char* when) {
            if (log_file == nullptr) {
                log_file = std::fopen("launcher_window.log", "w");
                if (log_file == nullptr) {
                    return;
                }
            }
            RECT client{};
            GetClientRect(hwnd, &client);
            RECT outer{};
            GetWindowRect(hwnd, &outer);
            int w = 0, h = 0, pw = 0, ph = 0, mx = 0, my = 0;
            SDL_GetWindowSize(chrome_window, &w, &h);
            SDL_GetWindowSizeInPixels(chrome_window, &pw, &ph);
            SDL_GetMouseState(&mx, &my);
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened_at).count();
            std::fprintf(log_file, "%6d ms frame %d %s: picture %ld x %ld, window %ld x %ld at %ld %ld, SDL %d x %d (%d x %d pixels), "
                "mouse by Windows %ld %ld, by SDL %d %d, dpi %u%s\n", ms, frames_up, when, client.right - client.left, client.bottom - client.top,
                outer.right - outer.left, outer.bottom - outer.top, outer.left, outer.top, w, h, pw, ph, cursor.x, cursor.y, mx, my,
                window_dpi(hwnd), IsZoomed(hwnd) ? ", maximized" : "");
            std::fflush(log_file);
        };
        // (0.11.4) maximized but not the work area's size (the picture the screen's, squeezed into a smaller window: the buttons lit above
        // the cursor): maximized again, properly
        if (!game_started && IsZoomed(hwnd) && !IsIconic(hwnd)) {
            static int fixes = 0;
            RECT outer{};
            GetWindowRect(hwnd, &outer);
            const RECT area = work_area(hwnd);
            if (fixes < 5 && (outer.right - outer.left) < (area.right - area.left) - 40) {
                fixes++;
                log_sizes("maximized but smaller than the screen: maximized again");
                std::printf("[window] maximized but %ld x %ld: maximized again\n", outer.right - outer.left, outer.bottom - outer.top);
                ShowWindow(hwnd, SW_RESTORE);
                ShowWindow(hwnd, SW_MAXIMIZE);
            }
        }
        if (!game_started && frames_up < 100000) {
            frames_up++;
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened_at).count();
            if (ms < 10000 && ms - last_logged_ms >= 500) {
                last_logged_ms = ms;
                log_sizes("the launcher");
            }
            (void)cycled;   // (the maximize and back as it opened is gone: it flashed, and didn't fix the buttons)
        }
    }
    {
        // (0.10.49) the Graphics tab's Windowed Size changed: the window takes the new size at once (not in full screen)
        static int size_seen = -1;
        const int fill = conker::windowed_size::fill_screen() ? 1 : 0;
        if (size_seen < 0) {
            size_seen = fill;
        }
        else if (fill != size_seen && !settings_full_screen && tile() == 0) {
            size_seen = fill;
            std::printf("[window] Windowed Size: %s\n", fill ? "Fill Screen" : "1600 x 900");
            if (fill) {
                ShowWindow(hwnd, SW_MAXIMIZE);
            }
            else if (!restored.load()) {
                centre_1600x900(hwnd);
            }
            else {
                // with the Windows title bar: 1600 x 900 inside it
                SDL_RestoreWindow(chrome_window);
                SDL_SetWindowSize(chrome_window, 1600, 900);
                SDL_SetWindowPosition(chrome_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
        }
    }
    if (restored.load()) {
        return;
    }
    if (game_started) {
        // the Windows title bar back for the game: the frame recomputed, the window grown round its picture so the picture keeps its
        // size (maximized, it stays maximized); in full screen RT64 brings the frame back when it goes back to windowed
        restored = true;
        max_hovered = false;
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & WS_CAPTION) == WS_CAPTION && !IsZoomed(hwnd) && !IsIconic(hwnd)) {
            RECT outer{};
            GetWindowRect(hwnd, &outer);
            AdjustWindowRectEx(&outer, (DWORD)style, FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            const RECT area = work_area(hwnd);
            if (outer.top < area.top) {
                OffsetRect(&outer, 0, area.top - outer.top);
            }
            SetWindowPos(hwnd, nullptr, outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        else {
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        }
        std::printf("[window] the game started: the Windows title bar is back\n");
        return;
    }
    // (once the mouse button is let go: the buttons act on the press)
    if (minimize_wanted.load() && (SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) == 0) {
        minimize_wanted = false;
        minimize_now();
    }
    if (toggle_wanted.exchange(false) && !settings_full_screen && !IsIconic(hwnd)) {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }
#else
    (void)game_started;
#endif
}
