#pragma once

// The build defines these for every target that links volt_platform; the guards
// keep the header usable if it is ever included without them.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <string_view>

namespace platform::windows
{

    // Everything the Win32 backend keeps per application. Filled in by
    // platform::initialize(), never modified afterwards, and owned by the
    // application object.
    struct state
    {
        // Module handle of the executable: needed to register window classes,
        // create windows and load resources.
        HINSTANCE instance = nullptr;

        // The window class every window is created from (see
        // <platform/windows/window.hpp>). Zero until registered.
        ATOM window_class = 0;

        // Thread that called platform::initialize(). Windows deliver messages
        // to the queue of the thread that created them, so windows must be
        // created on this thread and it is the only one that pumps.
        DWORD thread_id = 0;

        // Set by platform::wake(), waited on by platform::poll_events()
        // together with the message queue (a manual-reset event).
        //
        // An event rather than a message, because a message can be taken by
        // somebody else. A thread message is thrown away by every loop that
        // is not ours (DispatchMessage has no window to give it to), and
        // Windows runs such loops while a window is dragged or resized, in
        // menus and in message boxes. A message to a hidden window would
        // survive those loops, but they would dispatch it, so a wake that
        // arrives inside a loop entered outside poll_events() would be used
        // up there and the next poll_events() would sleep through it. The
        // event stays set until poll_events() itself takes it, and any
        // number of wakes are one set event, where messages would pile up
        // in the queue.
        HANDLE wake_event = nullptr;
    };

    // UTF-8 <-> UTF-16 conversion for crossing the Win32 API boundary. Invalid
    // sequences are replaced with U+FFFD.
    std::wstring to_wide(std::string_view utf8);
    std::string to_utf8(std::wstring_view wide);

    // Human-readable text for a Win32 error code, e.g. from GetLastError().
    std::string error_message(DWORD code);

} // namespace platform::windows
