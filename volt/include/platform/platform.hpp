#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <platform/windows/win32.hpp>
#elif defined(__linux__)
#include <platform/linux/wayland.hpp>
#else
#error "Volt has no platform backend for this operating system"
#endif

namespace platform
{

    // Per-application platform state. The platform layer keeps no globals: the
    // application owns one of these and passes it to every call. It is a set of
    // handles that never changes between initialize() and shutdown().
#if defined(_WIN32)
    using state = windows::state;
#elif defined(__linux__)
    using state = wayland::state;
#endif

    // Prepares the process for creating windows. The calling thread becomes the
    // platform thread: windows are created on it and poll_events() runs on it.
    std::expected<state, std::string> initialize();

    // Releases what initialize() acquired and resets the state. All windows
    // must be destroyed first. Safe on a state that was never initialized.
    void shutdown(state &state);

    // The application's only event pump. Sleeps until at least one OS event
    // arrives or wake() is called, hands every pending event to the window it
    // belongs to, and returns false once the application should exit (quit
    // requested, or the display server is gone). A true return says nothing
    // about why it woke: the caller checks for whatever work it queued.
    //
    // It must be called on the platform thread, and nothing else should pump:
    // windows receive their events from this call, they never read the queue
    // themselves.
    bool poll_events(const state &state);

    // Makes poll_events() return true promptly, from any thread, so that the
    // platform thread gets round to work the application queued for it
    // (creating and destroying windows, which only that thread may do). The
    // work must be queued before the call: a wake is a signal, not a message,
    // and any number of them before poll_events() looks come out as one
    // return. A wake is never lost, not even while the platform thread is
    // held in one of the window system's own loops (Windows runs one while a
    // window is dragged or resized): poll_events() returns once it is back.
    //
    // Valid between initialize() and shutdown(), which must not run
    // concurrently with it.
    void wake(const state &state);

    // Whether `url` is a web address open_url() will hand on: http:// or
    // https:// (either case) and a host, at most 2048 bytes, and nothing a
    // shell or a command line could read as more than one argument or as
    // something else (no spaces, quotes, angle brackets, backslashes,
    // backquotes or control characters). Bytes above ASCII pass, for
    // addresses with non-Latin letters. Portable, so it is tested anywhere.
    constexpr bool is_web_url(std::string_view url) noexcept
    {
        if (url.size() > 2048)
            return false;

        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        const auto starts_with = [&](std::string_view prefix) {
            if (url.size() < prefix.size())
                return false;
            for (std::size_t i = 0; i < prefix.size(); ++i)
            {
                if (lower(url[i]) != prefix[i])
                    return false;
            }
            return true;
        };

        std::size_t host = 0;
        if (starts_with("https://"))
            host = 8;
        else if (starts_with("http://"))
            host = 7;
        else
            return false;

        // The host runs to the path, query or fragment, and is not empty.
        std::size_t end = host;
        while (end < url.size() && url[end] != '/' && url[end] != '?' && url[end] != '#')
            ++end;
        if (end == host)
            return false;

        for (const char c : url)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return false;
            switch (c)
            {
            case ' ':
            case '"':
            case '<':
            case '>':
            case '\\':
            case '`':
                return false;
            default:
                break;
            }
        }
        return true;
    }

    // Opens `url` in the user's web browser: ShellExecuteW on Windows,
    // xdg-open on Linux. Only web addresses (is_web_url()); anything else is
    // refused without starting anything, so a link out of a library file
    // can never run a program or open a local file. Returns once the system
    // has taken the request, not when the browser is up. Any thread; it may
    // take a moment while the system finds the browser.
    std::expected<void, std::string> open_url(std::string_view url);

} // namespace platform
