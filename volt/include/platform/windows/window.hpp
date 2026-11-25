#pragma once

#include <math/vector.hpp>
#include <platform/windows/win32.hpp>

// The Common Item Dialog (<shobjidl.h>), only named here.
struct IFileDialog;

namespace volt::ui::detail
{
    struct window_data;
} // namespace volt::ui::detail

namespace platform
{

    // The Win32 side of a volt::ui::window. Owned by the window object and
    // reachable from the HWND (its GWLP_USERDATA points here), which is how
    // the window procedure finds the window a message belongs to.
    struct native_window
    {
        HWND handle = nullptr;
        HINSTANCE instance = nullptr;

        // The portable state and callbacks, owned by the window object.
        volt::ui::detail::window_data *data = nullptr;

        // Where the window was before it went fullscreen, to put it back.
        WINDOWPLACEMENT windowed_placement{};

        // Last known cursor position in the content area, and whether the
        // cursor is over it: Windows reports leaving but not entering.
        volt::math::vec2i cursor{};
        bool cursor_inside = false;

        // The window holds the mouse capture it took for a press, and has
        // not let it go itself: losing it now (WM_CAPTURECHANGED) is
        // another window taking it.
        bool captured = false;

        // What volt::ui::window::display() last found, and for which monitor.
        HMONITOR display_monitor = nullptr;
        bool display_hdr = false;
        float display_sdr_white = 80.0f;

        // First half of a character outside the Basic Multilingual Plane,
        // which arrives as two WM_CHAR messages.
        wchar_t high_surrogate = 0;

        // The file dialog showing for this window (<platform/file_dialog.hpp>),
        // while its Show() runs; null otherwise. Platform thread only.
        IFileDialog *file_dialog = nullptr;
    };

} // namespace platform

namespace platform::windows
{

    // Every window uses this one class. platform::initialize() registers it
    // and platform::shutdown() unregisters it.
    inline constexpr wchar_t window_class_name[] = L"volt.window";

    // The ID of the icon resource the window class shows, which the
    // executable provides (volt/src/volt.rc). An executable without one,
    // such as a test, gets the system's default icon.
    inline constexpr WORD app_icon_resource = 1;

    LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    // The file dialogs (src/platform/windows/file_dialog.cpp) are asked for
    // by posting these to the owner, from any thread; its window procedure
    // runs them on the platform thread. A posted message rather than a call
    // because the dialog must run on the window's own thread, and a window
    // message rather than a thread message because the nested loops of
    // Windows (and of other dialogs) dispatch the one and drop the other.
    //
    // file_dialog_message: lparam owns a heap request; the handler takes it.
    // close_file_dialog_message: closes the one showing, as if cancelled.
    inline constexpr UINT file_dialog_message = WM_APP + 1;
    inline constexpr UINT close_file_dialog_message = WM_APP + 2;

    // The window procedure's side of the two. `native` is null for a window
    // being created or destroyed: its request is answered as cancelled.
    void run_file_dialog(HWND window, native_window *native, LPARAM request);
    void end_file_dialog(native_window &native);

    // Answers, as cancelled, the requests posted to a window that is being
    // destroyed and has not taken them yet (they would be lost with it).
    void cancel_file_dialogs(HWND window);

} // namespace platform::windows
