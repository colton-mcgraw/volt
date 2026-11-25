#include <platform/platform.hpp>

#include "input.hpp"

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <system_error>

#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

// The process's environment, for posix_spawnp (unistd.h declares it only
// with _GNU_SOURCE).
extern char **environ;

namespace platform::wayland
{
    namespace
    {
        // Highest version of each global the backend is written against. A
        // newer compositor is asked to speak these, so it never sends an event
        // the listeners do not know.
        constexpr std::uint32_t compositor_version = 4;
        constexpr std::uint32_t shell_version = 3;
        constexpr std::uint32_t seat_version = 5;

        std::string errno_text()
        {
            return std::generic_category().message(errno);
        }

        // The compositor checks that the application is alive by pinging it. One
        // that does not answer gets its windows marked as not responding.
        void on_shell_ping(void *, xdg_wm_base *shell, std::uint32_t serial)
        {
            xdg_wm_base_pong(shell, serial);
        }

        constexpr xdg_wm_base_listener shell_listener{
            .ping = on_shell_ping,
        };

        // The compositor announces each global object it offers; bind the ones
        // the backend needs.
        void on_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
        {
            auto *result = static_cast<state *>(data);
            const auto is = [&](const wl_interface &wanted) { return std::strcmp(interface, wanted.name) == 0; };

            if (is(wl_compositor_interface))
            {
                result->compositor = static_cast<wl_compositor *>(wl_registry_bind(
                    registry, name, &wl_compositor_interface, std::min(version, compositor_version)));
            }
            else if (is(xdg_wm_base_interface))
            {
                result->shell = static_cast<xdg_wm_base *>(
                    wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, shell_version)));
                xdg_wm_base_add_listener(result->shell, &shell_listener, nullptr);
            }
            else if (is(wl_shm_interface))
            {
                result->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
            }
            else if (is(zxdg_decoration_manager_v1_interface))
            {
                result->decorations = static_cast<zxdg_decoration_manager_v1 *>(
                    wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
            }
            else if (is(wl_seat_interface) && result->input == nullptr)
            {
                // A compositor may offer several seats (sets of input
                // devices); the first one is the user's.
                result->input = create_input(static_cast<wl_seat *>(
                    wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, seat_version))));
            }
        }

        void on_global_remove(void *, wl_registry *, std::uint32_t)
        {
        }

        constexpr wl_registry_listener registry_listener{
            .global = on_global,
            .global_remove = on_global_remove,
        };
    } // namespace
} // namespace platform::wayland

namespace platform
{

    std::expected<state, std::string> initialize()
    {
        state result;

        result.display = wl_display_connect(nullptr);
        if (result.display == nullptr)
        {
            return std::unexpected(std::format(
                "wl_display_connect failed: {} (is WAYLAND_DISPLAY set?)", wayland::errno_text()));
        }

        // The registry is only needed to discover the globals, so it does not
        // outlive this function. The roundtrip blocks until the compositor has
        // announced everything it currently offers.
        wl_registry *registry = wl_display_get_registry(result.display);
        wl_registry_add_listener(registry, &wayland::registry_listener, &result);
        const int dispatched = wl_display_roundtrip(result.display);
        wl_registry_destroy(registry);

        if (dispatched < 0)
        {
            std::string error = std::format("wl_display_roundtrip failed: {}", wayland::errno_text());
            shutdown(result);
            return std::unexpected(std::move(error));
        }

        if (result.compositor == nullptr)
        {
            shutdown(result);
            return std::unexpected("the Wayland compositor does not offer wl_compositor");
        }

        if (result.shell == nullptr)
        {
            shutdown(result);
            return std::unexpected("the Wayland compositor does not offer xdg_wm_base, so it cannot show windows");
        }

        // Non-blocking: wake() must never wait, and a write that would
        // overflow the counter only means it is already set.
        result.wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (result.wake < 0)
        {
            std::string error = std::format("eventfd failed: {}", wayland::errno_text());
            shutdown(result);
            return std::unexpected(std::move(error));
        }

        result.thread = std::this_thread::get_id();
        return result;
    }

    void shutdown(state &state)
    {
        if (state.wake >= 0)
            close(state.wake);

        wayland::destroy_input(state.input);

        if (state.decorations != nullptr)
            zxdg_decoration_manager_v1_destroy(state.decorations);

        if (state.shm != nullptr)
            wl_shm_destroy(state.shm);

        if (state.shell != nullptr)
            xdg_wm_base_destroy(state.shell);

        if (state.compositor != nullptr)
            wl_compositor_destroy(state.compositor);

        if (state.display != nullptr)
            wl_display_disconnect(state.display);

        state = {};
    }

    bool poll_events(const state &state)
    {
        assert(std::this_thread::get_id() == state.thread && "poll_events must run on the thread that called initialize");
        
        wl_display *display = state.display;

        // This is libwayland's protocol for sharing the socket between threads:
        // announce the intent to read, read, then dispatch only our own queue.
        // Other readers (the Vulkan driver uses private queues on this same
        // connection) keep their events, and we never dispatch theirs.
        while (wl_display_prepare_read(display) != 0)
        {
            // Events are already queued for us; they must be handled first.
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        }

        // Send buffered requests. EAGAIN only means the socket is full for now.
        if (wl_display_flush(display) < 0 && errno != EAGAIN)
        {
            wl_display_cancel_read(display);
            return false;
        }

        // Sleep until the compositor sends something or wake() is called
        // (timeout -1 waits forever). A signal can end the wait early; that
        // just reports no events.
        pollfd sources[2] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0},
            {.fd = state.wake, .events = POLLIN, .revents = 0},
        };
        const bool ready = poll(sources, 2, -1) > 0;
        const pollfd &socket = sources[0];

        if (ready && (socket.revents & POLLIN) != 0)
        {
            if (wl_display_read_events(display) < 0)
                return false;
        }
        else
        {
            // Woken, or interrupted: the read that was announced must be
            // called off, or other readers of the connection would wait on it.
            wl_display_cancel_read(display);

            // The compositor went away.
            if (ready && (socket.revents & (POLLERR | POLLHUP)) != 0)
                return false;
        }

        // Taken whatever woke the thread: this call returns true either way,
        // and the caller then looks at its work. A wake after this read makes
        // the next poll return at once.
        if ((sources[1].revents & POLLIN) != 0)
        {
            std::uint64_t count = 0;
            [[maybe_unused]] const ssize_t taken = read(state.wake, &count, sizeof(count));
        }

        return wl_display_dispatch_pending(display) >= 0;
    }

    void wake(const state &state)
    {
        if (state.wake < 0)
            return;

        // Adds to the counter. EAGAIN means it is as full as it gets, which
        // is set all the same.
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t written = write(state.wake, &one, sizeof(one));
    }

    std::expected<void, std::string> open_url(std::string_view url)
    {
        if (!is_web_url(url))
            return std::unexpected(std::string("not a web address (http or https)"));

        // xdg-open finds the desktop's browser. It is started through a
        // shell that puts it in the background and ends at once, so the
        // shell is reaped here and xdg-open, which can take a while, is
        // left to init rather than becoming a zombie of this process. The
        // address is the shell's $1, never part of the command text, so
        // nothing in it is read by the shell.
        const std::string address(url);
        const char *arguments[] = {"sh", "-c", "xdg-open \"$1\" >/dev/null 2>&1 &", "sh", address.c_str(), nullptr};

        pid_t child = 0;
        const int spawned = posix_spawnp(&child, "sh", nullptr, nullptr, const_cast<char *const *>(arguments), environ);
        if (spawned != 0)
            return std::unexpected(std::format("cannot start xdg-open: {}", std::strerror(spawned)));

        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return std::unexpected(std::string("cannot start xdg-open"));
        return {};
    }

} // namespace platform
