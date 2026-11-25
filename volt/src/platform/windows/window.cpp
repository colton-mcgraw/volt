#include <app/ui/window.hpp>
#include <platform/windows/window.hpp>

#include "../scancode.hpp"

#include <shellapi.h>        // drag and drop; left out by WIN32_LEAN_AND_MEAN
#include <shellscalingapi.h> // GetDpiForMonitor

#include <algorithm>
#include <cassert>
#include <format>
#include <vector>

namespace platform::windows
{
    namespace
    {
        using volt::math::vec2i;
        using volt::ui::detail::emit;
        using volt::ui::detail::window_data;

        // The style bits that decide what frame a window has. Everything else
        // in a window's style (visible, maximized, ...) belongs to Windows.
        constexpr DWORD frame_styles = WS_OVERLAPPEDWINDOW | WS_POPUP;

        DWORD frame_style(const window_data &data)
        {
            if (data.fullscreen || !data.decorated)
                return WS_POPUP;

            DWORD style = WS_OVERLAPPEDWINDOW;
            if (!data.resizable)
                style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
            return style;
        }

        DWORD style_of(HWND window)
        {
            return static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE));
        }

        void set_frame_style(HWND window, DWORD frame)
        {
            const DWORD style = (style_of(window) & ~frame_styles) | frame;
            SetWindowLongW(window, GWL_STYLE, static_cast<LONG>(style));
        }

        // Makes Windows notice a style change without moving anything.
        void refresh_frame(HWND window)
        {
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
        }

        // The content area in screen coordinates.
        RECT content_rect(HWND window)
        {
            RECT rect{};
            GetClientRect(window, &rect);

            POINT origin{0, 0};
            ClientToScreen(window, &origin);
            OffsetRect(&rect, origin.x, origin.y);
            return rect;
        }

        // Moves and sizes the window so that its content area covers `content`
        // (screen coordinates): the frame goes around it.
        void place(HWND window, RECT content)
        {
            const DWORD extended_style = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
            AdjustWindowRectExForDpi(&content, style_of(window), FALSE, extended_style, GetDpiForWindow(window));

            SetWindowPos(window, nullptr, content.left, content.top, content.right - content.left,
                         content.bottom - content.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        // Applies data.resizable / data.decorated to an open window, keeping
        // its content area where it is.
        void update_frame(const native_window &native)
        {
            const HWND window = native.handle;
            const DWORD frame = frame_style(*native.data);
            if ((style_of(window) & frame_styles) == frame)
                return;

            const RECT content = content_rect(window);
            set_frame_style(window, frame);

            if (IsZoomed(window) || IsIconic(window))
                refresh_frame(window);
            else
                place(window, content);
        }

        volt::ui::modifiers current_modifiers()
        {
            using volt::ui::modifiers;

            const auto down = [](int virtual_key) { return (GetKeyState(virtual_key) & 0x8000) != 0; };
            const auto toggled = [](int virtual_key) { return (GetKeyState(virtual_key) & 1) != 0; };

            modifiers result = modifiers::none;
            if (down(VK_SHIFT))
                result |= modifiers::shift;
            if (down(VK_CONTROL))
                result |= modifiers::control;
            if (down(VK_MENU))
                result |= modifiers::alt;
            if (down(VK_LWIN) || down(VK_RWIN))
                result |= modifiers::super;
            if (toggled(VK_CAPITAL))
                result |= modifiers::caps_lock;
            if (toggled(VK_NUMLOCK))
                result |= modifiers::num_lock;
            return result;
        }

        // `extended` is the flag Windows sets for the keys the enhanced
        // keyboard added, whose scancodes carry an E0 prefix on the wire.
        volt::ui::key translate_key(unsigned scancode, bool extended)
        {
            using enum volt::ui::key;

            if (extended)
            {
                switch (scancode)
                {
                case 0x1C: return kp_enter;
                case 0x1D: return right_control;
                case 0x35: return kp_divide;
                case 0x37: return print_screen;
                case 0x38: return right_alt;
                case 0x45: return num_lock;
                case 0x46: return pause; // Ctrl+Pause
                case 0x47: return home;
                case 0x48: return up;
                case 0x49: return page_up;
                case 0x4B: return left;
                case 0x4D: return right;
                case 0x4F: return end;
                case 0x50: return down;
                case 0x51: return page_down;
                case 0x52: return insert;
                case 0x53: return del;
                case 0x5B: return left_super;
                case 0x5C: return right_super;
                case 0x5D: return menu;
                default: return unknown;
                }
            }

            // Two places where Windows departs from the plain table: Pause
            // shares Num Lock's code (told apart by the extended flag), and
            // Alt+Print Screen has a code of its own.
            if (scancode == 0x45)
                return pause;
            if (scancode == 0x54)
                return print_screen;

            return key_from_set1(scancode);
        }

        // Mouse messages pack a signed position into the two halves of LPARAM.
        vec2i point_from(LPARAM lparam)
        {
            return {static_cast<short>(lparam & 0xFFFF), static_cast<short>((lparam >> 16) & 0xFFFF)};
        }

        // WM_MOUSELEAVE has to be asked for, once per visit.
        void track_leave(HWND window)
        {
            TRACKMOUSEEVENT track{
                .cbSize = sizeof(TRACKMOUSEEVENT),
                .dwFlags = TME_LEAVE,
                .hwndTrack = window,
                .dwHoverTime = 0,
            };
            TrackMouseEvent(&track);
        }

        // The cursor is over the window's content area, and no other window
        // covers it there.
        bool cursor_over(HWND window)
        {
            POINT point{};
            if (!GetCursorPos(&point) || WindowFromPoint(point) != window)
                return false;

            RECT client{};
            GetClientRect(window, &client);
            ScreenToClient(window, &point);
            return PtInRect(&client, point) != FALSE;
        }

        void on_button(native_window &native, volt::ui::mouse_button button, volt::ui::action action, WPARAM wparam)
        {
            const HWND window = native.handle;
            const window_data &data = *native.data;

            // Capturing keeps the messages coming while a button is held and
            // the cursor is dragged outside the window, so a press always gets
            // its release.
            //
            // When the last button comes up somewhere else, over the desktop
            // or over another of the application's windows, nothing reliably
            // tells this window that the cursor has gone: leave tracking does
            // not follow the cursor while it is captured. So the leave is
            // worked out here, and reported after the release, as it happened.
            // Otherwise the tracking is asked for again, for the visit that
            // goes on.
            constexpr WPARAM any_button = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
            bool left = false;
            if (action == volt::ui::action::press)
            {
                SetCapture(window);
                native.captured = true;
            }
            else if ((wparam & any_button) == 0)
            {
                // Cleared first: the WM_CAPTURECHANGED that ReleaseCapture
                // sends is this window letting go, not losing the capture.
                native.captured = false;
                ReleaseCapture();
                if (native.cursor_inside && !cursor_over(window))
                {
                    native.cursor_inside = false;
                    left = true;
                }
                else if (native.cursor_inside)
                {
                    track_leave(window);
                }
            }
            const vec2i last = native.cursor;

            emit(data.on_mouse, button, action, current_modifiers());
            if (left)
                emit(data.on_leave, last);
        }

        // The scale of the monitor the window is on, as Windows will give it
        // the window. MonitorFromWindow takes a minimized window's restored
        // rectangle, so this is right for one coming back, too.
        float monitor_scale(HWND window)
        {
            UINT dpi_x = 0;
            UINT dpi_y = 0;
            const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) != S_OK || dpi_x == 0)
                return static_cast<float>(GetDpiForWindow(window)) / USER_DEFAULT_SCREEN_DPI;
            return static_cast<float>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
        }

        volt::ui::screen_rect from_rect(const RECT &r)
        {
            return {{static_cast<int>(r.left), static_cast<int>(r.top)},
                    {static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top)}};
        }

        // Moves the window, and makes it smaller where it must, so that all
        // of it, frame included, is inside the work area of the monitor it
        // is mostly on (or nearest to). Returns whether it changed. The
        // outer rectangle includes the invisible resizing borders Windows
        // puts around a frame, so the visible frame ends a few pixels short
        // of the work area's edge rather than under the taskbar.
        bool keep_in_work_area(HWND window)
        {
            RECT outer{};
            if (!GetWindowRect(window, &outer))
                return false;
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(MONITORINFO);
            if (!GetMonitorInfoW(MonitorFromRect(&outer, MONITOR_DEFAULTTONEAREST), &monitor))
                return false;

            const volt::ui::screen_rect now = from_rect(outer);
            const volt::ui::screen_rect fitted = volt::ui::fit_inside(now, from_rect(monitor.rcWork));
            if (fitted == now)
                return false;
            SetWindowPos(window, nullptr, fitted.position.x(), fitted.position.y(), fitted.size.x(), fitted.size.y(),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return true;
        }

        // The smallest outer size that leaves the content area `min`
        // (logical) at the window's DPI; zero on an axis without a minimum.
        vec2i min_outer_size(HWND window, const window_data &data)
        {
            const UINT dpi = GetDpiForWindow(window);
            const vec2i content = volt::ui::logical_to_window(data.min_size, static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI);
            if (content.x() <= 0 && content.y() <= 0)
                return {};

            RECT frame{0, 0, content.x(), content.y()};
            const DWORD extended_style = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
            AdjustWindowRectExForDpi(&frame, style_of(window), FALSE, extended_style, dpi);
            return {content.x() > 0 ? static_cast<int>(frame.right - frame.left) : 0,
                    content.y() > 0 ? static_cast<int>(frame.bottom - frame.top) : 0};
        }

        // Grows the content area to the minimum where it is smaller,
        // keeping its corner, and back inside the work area if that pushed
        // it out. Not while maximized, minimized or fullscreen: those sizes
        // are not the window's own.
        void grow_to_minimum(HWND window, const window_data &data)
        {
            if (data.fullscreen || IsZoomed(window) || IsIconic(window))
                return;
            const vec2i min = volt::ui::logical_to_window(
                data.min_size, static_cast<float>(GetDpiForWindow(window)) / USER_DEFAULT_SCREEN_DPI);
            RECT content = content_rect(window);
            const bool narrow = content.right - content.left < min.x();
            const bool short_ = content.bottom - content.top < min.y();
            if (!narrow && !short_)
                return;
            if (narrow)
                content.right = content.left + min.x();
            if (short_)
                content.bottom = content.top + min.y();
            place(window, content);
            keep_in_work_area(window);
        }

        void on_size(window_data &data, WPARAM kind, vec2i size)
        {
            const bool minimized = kind == SIZE_MINIMIZED;
            const bool maximized = kind == SIZE_MAXIMIZED;
            const bool was_minimized = std::exchange(data.minimized, minimized);
            const bool was_maximized = std::exchange(data.maximized, maximized);

            // A minimized window has no content area; keep the size it will
            // come back with.
            const bool resized = !minimized && size != data.size;
            if (resized)
                data.size = size;

            if (minimized != was_minimized)
                emit(data.on_minimize, minimized);
            if (maximized != was_maximized)
                emit(data.on_maximize, maximized);
            if (kind == SIZE_RESTORED && (was_minimized || was_maximized))
                emit(data.on_restore);
            if (resized)
                emit(data.on_resize, size);
        }

        // Asks the display configuration API about `monitor`. It describes
        // the desktop as paths from a source (a slice of the desktop, which
        // is what a monitor handle stands for) to a target (the physical
        // display); the color state belongs to the target.
        volt::ui::display_info query_display(HMONITOR monitor)
        {
            volt::ui::display_info result;

            MONITORINFOEXW info{};
            info.cbSize = sizeof(MONITORINFOEXW);
            if (!GetMonitorInfoW(monitor, &info))
                return result;

            UINT32 path_count = 0;
            UINT32 mode_count = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
                return result;

            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                   nullptr) != ERROR_SUCCESS)
                return result;
            paths.resize(path_count);

            for (const DISPLAYCONFIG_PATH_INFO &path : paths)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = path.sourceInfo.adapterId;
                source.header.id = path.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                    std::wstring_view(source.viewGdiDeviceName) != info.szDevice)
                    continue;

                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
                color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                color.header.size = sizeof(color);
                color.header.adapterId = path.targetInfo.adapterId;
                color.header.id = path.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS || !color.advancedColorEnabled)
                    return result;

                result.hdr = true;

                // In units of a thousandth of 80 nits.
                DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
                white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                white.header.size = sizeof(white);
                white.header.adapterId = path.targetInfo.adapterId;
                white.header.id = path.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
                    result.sdr_white = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;

                return result;
            }

            return result;
        }

        void on_drop(const window_data &data, HDROP drop)
        {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

            std::vector<std::string> paths;
            paths.reserve(count);
            for (UINT i = 0; i < count; ++i)
            {
                // The length excludes the terminator, the buffer size includes it.
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1);
                paths.push_back(to_utf8(path));
            }
            DragFinish(drop);

            emit(data.on_drop, paths);
        }
    } // namespace

    // Runs for every message of every window, called by DispatchMessageW in
    // platform::poll_events() and by Windows itself, directly and from its
    // nested loops (dragging, resizing, menus).
    //
    // A callback may destroy the window, which frees `native`. So each case
    // finishes with `native` before it emits, and afterwards only touches
    // `data`, which lives in the window object.
    LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        // Null while the window is being created or destroyed: those messages
        // are of no interest, create() and destroy() know what they are doing.
        auto *native = reinterpret_cast<native_window *>(GetWindowLongPtrW(window, GWLP_USERDATA));

        // Before the check below: a request owns memory and has a callback
        // waiting, so it is answered whatever state the window is in. The
        // dialog's loop runs inside this call, for as long as it is up.
        if (message == file_dialog_message)
        {
            run_file_dialog(window, native, lparam);
            return 0;
        }
        if (message == close_file_dialog_message)
        {
            if (native != nullptr)
                end_file_dialog(*native);
            return 0;
        }

        if (native == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        window_data &data = *native->data;

        switch (message)
        {
        case WM_CLOSE:
            // Not passed on: the default would destroy the window behind the
            // application's back.
            emit(data.on_close);
            return 0;

        case WM_SIZE:
            // Coming back from minimized. The scale of the window's monitor
            // may have changed meanwhile (the user rescaled it, or the
            // window's place is now on another monitor), and Windows may
            // tell the window only after this, with WM_DPICHANGED. Reported
            // here, before the restore and the resize, the scale is what the
            // interface lays out the first frame with; WM_DPICHANGED then
            // finds it reported and only sizes the window.
            if (data.minimized && wparam != SIZE_MINIMIZED)
            {
                const float scale = monitor_scale(window);
                if (std::exchange(data.scale, scale) != scale)
                {
                    emit(data.on_scale, scale);
                    if (GetWindowLongPtrW(window, GWLP_USERDATA) == 0)
                        return 0; // the callback destroyed the window
                }
            }
            on_size(data, wparam, point_from(lparam));
            return 0;

        case WM_GETMINMAXINFO:
        {
            // The minimum content size, at the window's DPI, with the
            // frame. Asked for whenever the window is about to change size,
            // by the user or by SetWindowPos, so it follows the window from
            // monitor to monitor. Not while fullscreen, which covers the
            // monitor whatever the minimum says.
            if (data.fullscreen)
                break;
            const vec2i min = min_outer_size(window, data);
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
            if (min.x() > 0)
                info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, static_cast<LONG>(min.x()));
            if (min.y() > 0)
                info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, static_cast<LONG>(min.y()));
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            // Another window took the mouse while a button was held here:
            // a system dialog, a drag-and-drop source, a debugger. The focus
            // may well stay, so nothing else would release what the press
            // holds. The buttons' releases will not come, so the press is
            // called off (capture_lost_callback), not completed. The window
            // letting go itself clears `captured` first.
            if (!native->captured || reinterpret_cast<HWND>(lparam) == window)
                return 0;
            native->captured = false;

            // As on the last release: leave tracking did not follow the
            // cursor while it was captured.
            bool left = false;
            if (native->cursor_inside && !cursor_over(window))
            {
                native->cursor_inside = false;
                left = true;
            }
            else if (native->cursor_inside)
            {
                track_leave(window);
            }
            const vec2i last = native->cursor;

            emit(data.on_capture_lost);
            if (left)
                emit(data.on_leave, last);
            return 0;
        }

        case WM_MOVE:
        {
            // Minimized windows are parked far off screen; that is not a move.
            const vec2i position = point_from(lparam);
            if (IsIconic(window) || position == data.position)
                return 0;

            data.position = position;
            emit(data.on_move, position);
            return 0;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            data.focused = message == WM_SETFOCUS;
            emit(data.on_focus, data.focused);
            return 0;

        case WM_DPICHANGED:
        {
            // The window moved to a monitor with a different scale, or its
            // monitor was rescaled. Windows suggests a rectangle that keeps
            // its apparent size.
            //
            // The scale is reported first, then the window takes the
            // rectangle, which reports the resize (WM_SIZE, synchronously,
            // from inside SetWindowPos). In that order, whoever lays out on
            // the resize already has the scale to lay out with; and a
            // rectangle that happens to keep the size still leaves the scale
            // reported. Both happen before this message returns.
            // The low word is the new DPI (the high word repeats it).
            const float scale = static_cast<float>(wparam & 0xFFFF) / USER_DEFAULT_SCREEN_DPI;
            const RECT suggested = *reinterpret_cast<const RECT *>(lparam);

            if (std::exchange(data.scale, scale) != scale)
                emit(data.on_scale, scale);

            // The callback may have destroyed the window, which unlinks it
            // first, or made it fullscreen. A fullscreen window keeps
            // covering its monitor.
            if (GetWindowLongPtrW(window, GWLP_USERDATA) == 0 || data.fullscreen)
                return 0;

            SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                         suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        // ---- mouse -------------------------------------------------------

        case WM_MOUSEMOVE:
        {
            const vec2i position = point_from(lparam);
            const bool entered = !std::exchange(native->cursor_inside, true);
            native->cursor = position;

            if (entered)
            {
                track_leave(window);
                emit(data.on_enter, position);
            }

            emit(data.on_cursor, position);
            return 0;
        }

        case WM_MOUSELEAVE:
        {
            // on_button may have reported this leave already.
            if (!std::exchange(native->cursor_inside, false))
                return 0;

            const vec2i position = native->cursor;
            emit(data.on_leave, position);
            return 0;
        }

        case WM_LBUTTONDOWN:
            on_button(*native, volt::ui::mouse_button::left, volt::ui::action::press, wparam);
            return 0;
        case WM_LBUTTONUP:
            on_button(*native, volt::ui::mouse_button::left, volt::ui::action::release, wparam);
            return 0;
        case WM_RBUTTONDOWN:
            on_button(*native, volt::ui::mouse_button::right, volt::ui::action::press, wparam);
            return 0;
        case WM_RBUTTONUP:
            on_button(*native, volt::ui::mouse_button::right, volt::ui::action::release, wparam);
            return 0;
        case WM_MBUTTONDOWN:
            on_button(*native, volt::ui::mouse_button::middle, volt::ui::action::press, wparam);
            return 0;
        case WM_MBUTTONUP:
            on_button(*native, volt::ui::mouse_button::middle, volt::ui::action::release, wparam);
            return 0;

        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        {
            // The high word says which of the two side buttons, the low word
            // holds the same button flags as the other mouse messages.
            const auto button = ((wparam >> 16) & 0xFFFF) == XBUTTON1 ? volt::ui::mouse_button::back
                                                                       : volt::ui::mouse_button::forward;
            const auto action = message == WM_XBUTTONDOWN ? volt::ui::action::press : volt::ui::action::release;
            on_button(*native, button, action, wparam & 0xFFFF);
            return TRUE; // these two are documented to return TRUE when handled
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            // The high word is a signed distance, WHEEL_DELTA per notch.
            // Positive is away from the user, or to the right.
            const double notches = static_cast<short>((wparam >> 16) & 0xFFFF) / static_cast<double>(WHEEL_DELTA);
            if (message == WM_MOUSEWHEEL)
                emit(data.on_scroll, 0.0, notches);
            else
                emit(data.on_scroll, notches, 0.0);
            return 0;
        }

        // ---- keyboard ----------------------------------------------------

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        {
            // LPARAM: bits 16-23 scancode, 24 extended key, 30 was already
            // down, 31 is being released.
            const bool extended = ((lparam >> 24) & 1) != 0;
            const bool repeated = ((lparam >> 30) & 1) != 0;
            const bool released = ((lparam >> 31) & 1) != 0;

            // Synthesized key presses can arrive without a scancode.
            auto scancode = static_cast<unsigned>((lparam >> 16) & 0xFF);
            if (scancode == 0)
                scancode = MapVirtualKeyW(static_cast<UINT>(wparam), MAPVK_VK_TO_VSC);

            const auto action = released   ? volt::ui::action::release
                                : repeated ? volt::ui::action::repeat
                                           : volt::ui::action::press;

            emit(data.on_key, translate_key(scancode, extended), static_cast<int>(scancode | (extended ? 0x100u : 0u)),
                 action, current_modifiers());

            // The "system" variants are the keys pressed with Alt held, and
            // F10. The default handling turns them into Alt+F4, Alt+Space
            // and friends, which stay; but it also turns F10, and Alt
            // pressed and let go on its own, into keyboard access to the
            // window's menu (SC_KEYMENU). These windows have no native
            // menu: their menus are the interface's, which F10 opens itself.
            // Passed on, either key would put the window in the system's
            // menu mode, which takes the arrow keys and Enter away from the
            // interface until Escape. So those two stop here.
            if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
            {
                if (wparam == VK_F10 || wparam == VK_MENU)
                    return 0;
                break;
            }
            return 0;
        }

        case WM_SYSCHAR:
            // Alt with a character key. The default handling looks for a
            // native menu with that mnemonic and beeps when there is none,
            // which here is always. Alt+Space alone goes on: it opens the
            // window menu (move, size, close).
            if (wparam != L' ')
                return 0;
            break;

        case WM_CHAR:
        {
            // UTF-16, one unit per message: characters outside the Basic
            // Multilingual Plane arrive as a surrogate pair.
            const auto unit = static_cast<char32_t>(wparam);
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                native->high_surrogate = static_cast<wchar_t>(unit);
                return 0;
            }

            char32_t codepoint = unit;
            const char32_t high = std::exchange(native->high_surrogate, L'\0');
            if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                if (high == 0)
                    return 0; // unpaired
                codepoint = 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00);
            }

            // Enter, Backspace, Escape and the Ctrl+letter combinations show
            // up here as control characters. They are keys, not text.
            if (codepoint >= 0x20 && codepoint != 0x7F)
                emit(data.on_char, codepoint);
            return 0;
        }

        case WM_DROPFILES:
            on_drop(data, reinterpret_cast<HDROP>(wparam));
            return 0;

        default:
            break;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }

} // namespace platform::windows

namespace volt::ui
{
    using namespace platform::windows;

    window::window() = default;

    window::~window()
    {
        destroy();
    }

    std::expected<void, std::string> window::create(const platform::state &platform,
                                                    const window_configuration &config)
    {
        assert(GetCurrentThreadId() == platform.thread_id && "windows must be created on the platform thread");

        if (m_native)
            return std::unexpected("the window is already open");

        m_data.title = config.title;
        m_data.min_size = config.min_size;
        m_data.resizable = config.resizable;
        m_data.decorated = config.decorated;
        m_data.fullscreen = false; // entered below, once there is a window
        m_data.visible = false;
        m_data.focused = false;
        m_data.maximized = false;
        m_data.minimized = false;

        auto native = std::make_unique<platform::native_window>();
        native->instance = platform.instance;
        native->data = &m_data;

        // Created at whatever position and size Windows picks: the frame's
        // thickness and the content's scale depend on the monitor the window
        // lands on, so the real size is set once that is known. No messages reach the window
        // procedure's switch until the handle is linked to `native` below, so
        // no callback runs while the window is half built.
        native->handle = CreateWindowExW(0, window_class_name, to_wide(config.title).c_str(), frame_style(m_data),
                                         CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
                                         nullptr, platform.instance, nullptr);
        if (native->handle == nullptr)
        {
            const DWORD code = GetLastError();
            return std::unexpected(std::format("CreateWindowExW failed ({}): {}", code, error_message(code)));
        }

        const HWND handle = native->handle;

        // config.size is logical: the scale of the monitor the window is on
        // turns it into pixels. Rounded to the nearest pixel, and no smaller
        // than the minimum (which WM_GETMINMAXINFO cannot enforce yet: the
        // window is not linked).
        const auto place_content = [&](RECT content, UINT dpi) {
            const float scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
            const vec2i size = volt::ui::logical_to_window(config.size, scale);
            const vec2i min = volt::ui::logical_to_window(config.min_size, scale);
            content.right = content.left + std::max(size.x(), min.x());
            content.bottom = content.top + std::max(size.y(), min.y());
            place(handle, content);
        };

        RECT content = content_rect(handle);
        if (config.position)
        {
            content.left = config.position->x();
            content.top = config.position->y();
        }
        const UINT created_dpi = GetDpiForWindow(handle);
        place_content(content, created_dpi);

        // Moving the window to config.position may have put it on a monitor
        // with another scale. Windows announces that with WM_DPICHANGED,
        // which goes unanswered while the window is unlinked, so size it
        // again for where it landed, keeping the corner. Once is enough,
        // except astride two monitors, where the new size may tip the window
        // back onto the first; it then stays a little off, as a window the
        // user drags there would.
        if (const UINT landed_dpi = GetDpiForWindow(handle); landed_dpi != created_dpi)
            place_content(content_rect(handle), landed_dpi);

        // All of it inside the work area of the monitor it landed on: a
        // given position may be a remembered one from a monitor that has
        // gone or been rearranged, a chosen one (CW_USEDEFAULT cascades
        // down and to the right with every window made) may run under the
        // taskbar, and a size may be more than the monitor has at its
        // scale. Should that move it to a monitor of another scale, it is
        // sized for that one and fitted again; the second fit stands.
        const UINT placed_dpi = GetDpiForWindow(handle);
        if (keep_in_work_area(handle) && GetDpiForWindow(handle) != placed_dpi)
        {
            place_content(content_rect(handle), GetDpiForWindow(handle));
            keep_in_work_area(handle);
        }

        m_native = std::move(native);

        if (config.fullscreen)
            set_fullscreen(true);

        content = content_rect(handle);
        m_data.position = {static_cast<int>(content.left), static_cast<int>(content.top)};
        m_data.size = {static_cast<int>(content.right - content.left), static_cast<int>(content.bottom - content.top)};
        m_data.scale = static_cast<float>(GetDpiForWindow(handle)) / USER_DEFAULT_SCREEN_DPI;

        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m_native.get()));
        DragAcceptFiles(handle, TRUE);
        return {};
    }

    void window::destroy()
    {
        if (!m_native)
            return;

        // Unlink first: DestroyWindow sends a last burst of messages (focus
        // lost, hidden, ...) that should not turn into callbacks.
        const HWND handle = m_native->handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        m_native.reset();

        // File dialogs asked for and not yet begun would go with the
        // window's queue, their callbacks never run.
        cancel_file_dialogs(handle);
        DestroyWindow(handle);

        // A closed window is on no monitor. Not reported: nothing is, now.
        m_data.scale = 1.0f;
        m_data.fullscreen = false;
        m_data.visible = false;
        m_data.focused = false;
        m_data.maximized = false;
        m_data.minimized = false;
    }

    void window::show()
    {
        if (!m_native)
            return;

        m_data.visible = true;
        ShowWindow(m_native->handle, SW_SHOW);
    }

    void window::hide()
    {
        if (!m_native)
            return;

        m_data.visible = false;
        ShowWindow(m_native->handle, SW_HIDE);
    }

    void window::maximize()
    {
        if (!m_native)
            return;

        m_data.visible = true;
        ShowWindow(m_native->handle, SW_MAXIMIZE);
    }

    void window::minimize()
    {
        if (!m_native)
            return;

        m_data.visible = true;
        ShowWindow(m_native->handle, SW_MINIMIZE);
    }

    void window::restore()
    {
        if (!m_native)
            return;

        m_data.visible = true;
        ShowWindow(m_native->handle, SW_RESTORE);
    }

    void window::focus()
    {
        if (!m_native)
            return;

        const HWND handle = m_native->handle;
        m_data.visible = true;

        // Both activate the window as well; SW_RESTORE brings a minimized
        // window back to how it was, maximized included.
        if (IsIconic(handle))
            ShowWindow(handle, SW_RESTORE);
        else if (!IsWindowVisible(handle))
            ShowWindow(handle, SW_SHOW);

        // The focus callbacks of this window and of the one losing the
        // keyboard may already have run, and closed this one.
        if (!m_native || m_native->handle != handle)
            return;

        // Activates the window, which gives it the keyboard (WM_SETFOCUS).
        // Windows refuses while another application has the foreground and
        // the user has not given this one input lately, so that windows do
        // not jump in front of what the user is doing; it flashes the
        // taskbar button instead, which is the right outcome for a window
        // the user did not ask for.
        SetForegroundWindow(handle);
    }

    void window::set_title(const std::string &title)
    {
        if (!m_native)
            return;

        m_data.title = title;
        SetWindowTextW(m_native->handle, to_wide(title).c_str());
    }

    void window::set_resizable(bool resizable)
    {
        if (!m_native)
            return;

        // While fullscreen there is no frame; the new setting takes effect on
        // the way out. Same for set_decorated.
        m_data.resizable = resizable;
        update_frame(*m_native);
    }

    void window::set_decorated(bool decorated)
    {
        if (!m_native)
            return;

        m_data.decorated = decorated;
        update_frame(*m_native);
    }

    // Borderless fullscreen: the window loses its frame and covers the monitor
    // it is on. The display mode is left alone.
    void window::set_fullscreen(bool fullscreen)
    {
        if (!m_native || m_data.fullscreen == fullscreen)
            return;

        const HWND handle = m_native->handle;
        WINDOWPLACEMENT &placement = m_native->windowed_placement;
        m_data.fullscreen = fullscreen;

        if (fullscreen)
        {
            placement.length = sizeof(WINDOWPLACEMENT);
            GetWindowPlacement(handle, &placement);

            MONITORINFO monitor{};
            monitor.cbSize = sizeof(MONITORINFO);
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT &area = monitor.rcMonitor;

            set_frame_style(handle, frame_style(m_data));
            SetWindowPos(handle, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            // The placement also brings back a maximized state, but restoring
            // it must not show a window that is hidden.
            if (!IsWindowVisible(handle))
                placement.showCmd = SW_HIDE;

            set_frame_style(handle, frame_style(m_data));
            SetWindowPlacement(handle, &placement);
            refresh_frame(handle);
        }
    }

    display_info window::display(bool refresh)
    {
        if (!m_native)
            return {};

        const HMONITOR monitor = MonitorFromWindow(m_native->handle, MONITOR_DEFAULTTONEAREST);
        if (refresh || monitor != m_native->display_monitor)
        {
            const display_info found = query_display(monitor);
            m_native->display_monitor = monitor;
            m_native->display_hdr = found.hdr;
            m_native->display_sdr_white = found.sdr_white;
        }

        return {.hdr = m_native->display_hdr, .sdr_white = m_native->display_sdr_white};
    }

    void window::set_position(const math::vec2i &position)
    {
        if (!m_native || m_data.fullscreen)
            return;

        RECT content = content_rect(m_native->handle);
        OffsetRect(&content, position.x() - content.left, position.y() - content.top);
        place(m_native->handle, content);
    }

    void window::set_size(const math::vec2i &size)
    {
        if (!m_native || m_data.fullscreen)
            return;

        RECT content = content_rect(m_native->handle);
        content.right = content.left + size.x();
        content.bottom = content.top + size.y();
        place(m_native->handle, content);
    }

    void window::set_min_size(const math::vec2i &logical)
    {
        m_data.min_size = {std::max(logical.x(), 0), std::max(logical.y(), 0)};
        if (!m_native)
            return;

        // Windows asks for the limit (WM_GETMINMAXINFO) whenever the size
        // changes; one that is under it already is grown now.
        grow_to_minimum(m_native->handle, m_data);
    }

    std::optional<monitor_area> monitor_at(const platform::state &, std::optional<math::vec2i> point)
    {
        const HMONITOR monitor = point ? MonitorFromPoint(POINT{point->x(), point->y()}, MONITOR_DEFAULTTONEAREST)
                                       : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(MONITORINFO);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
            return std::nullopt;

        UINT dpi_x = 0;
        UINT dpi_y = 0;
        float scale = 1.0f;
        if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x != 0)
            scale = static_cast<float>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
        return monitor_area{.work = from_rect(info.rcWork), .scale = scale};
    }

} // namespace volt::ui
