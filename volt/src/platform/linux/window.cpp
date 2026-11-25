#include <app/ui/window.hpp>
#include <platform/linux/window.hpp>

#include "input.hpp"

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <sys/mman.h>
#include <unistd.h>

namespace platform::wayland
{
    namespace
    {
        using volt::math::vec2i;
        using volt::ui::detail::emit;
        using volt::ui::detail::window_data;

        // ---- placeholder content -----------------------------------------
        //
        // A Wayland window only exists on screen while it has a buffer of
        // pixels attached, and its size is the size of that buffer. Until a
        // renderer provides the buffers, windows show a black one, so that
        // they can be seen and sized. This whole section goes away then.

        // The compositor is done with the buffer.
        void on_buffer_release(void *, wl_buffer *buffer)
        {
            wl_buffer_destroy(buffer);
        }

        constexpr wl_buffer_listener buffer_listener{
            .release = on_buffer_release,
        };

        wl_buffer *create_black_buffer(wl_shm *shm, vec2i size)
        {
            const int stride = size.x() * 4;
            const int bytes = stride * size.y();

            // An anonymous file in memory, shared with the compositor. Growing
            // it fills it with zeros, which is black in XRGB.
            const int fd = memfd_create("volt-window", MFD_CLOEXEC);
            if (fd < 0)
                return nullptr;

            wl_buffer *buffer = nullptr;
            if (ftruncate(fd, bytes) == 0)
            {
                wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes);
                buffer = wl_shm_pool_create_buffer(pool, 0, size.x(), size.y(), stride, WL_SHM_FORMAT_XRGB8888);
                wl_shm_pool_destroy(pool);
                wl_buffer_add_listener(buffer, &buffer_listener, nullptr);
            }

            close(fd);
            return buffer;
        }

        // Gives the window content of its current size, which maps it or
        // resizes it. Only allowed once the window is configured.
        void present(const native_window &native)
        {
            const vec2i size = native.data->size;
            if (native.platform.shm == nullptr || size.x() <= 0 || size.y() <= 0)
                return;

            wl_buffer *buffer = create_black_buffer(native.platform.shm, size);
            if (buffer == nullptr)
                return;

            wl_surface_attach(native.surface, buffer, 0, 0);
            wl_surface_damage(native.surface, 0, 0, size.x(), size.y());
            wl_surface_commit(native.surface);
        }

        // ---- window state ------------------------------------------------

        // A window that may not be resized is one whose smallest and largest
        // size are the same; a resizable one has the minimum it was given
        // (logical, and the surface's units are logical here) and no
        // maximum. Zero means no limit.
        void apply_size_limits(const native_window &native)
        {
            const window_data &data = *native.data;
            const vec2i min = data.resizable ? vec2i{std::max(data.min_size.x(), 0), std::max(data.min_size.y(), 0)}
                                             : native.windowed_size;
            const vec2i max = data.resizable ? vec2i{} : native.windowed_size;
            xdg_toplevel_set_min_size(native.toplevel, min.x(), min.y());
            xdg_toplevel_set_max_size(native.toplevel, max.x(), max.y());
        }

        void apply_decoration(const native_window &native)
        {
            if (native.decoration == nullptr)
                return;

            zxdg_toplevel_decoration_v1_set_mode(native.decoration,
                                                 native.data->decorated
                                                     ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                     : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        }

        // Sends everything the compositor keeps about the window. It forgets
        // all of it when the window is hidden, so this runs on every show().
        void apply_state(const native_window &native)
        {
            const window_data &data = *native.data;

            xdg_toplevel_set_title(native.toplevel, data.title.c_str());
            xdg_toplevel_set_app_id(native.toplevel, "volt");
            apply_size_limits(native);
            apply_decoration(native);

            if (data.fullscreen)
                xdg_toplevel_set_fullscreen(native.toplevel, nullptr);
            if (data.maximized)
                xdg_toplevel_set_maximized(native.toplevel);
        }

        // Size limits take effect with the next commit of the surface.
        void commit_if_mapped(const native_window &native)
        {
            if (native.configured && native.data->visible)
                wl_surface_commit(native.surface);
        }

        void release(native_window &native)
        {
            if (native.decoration != nullptr)
                zxdg_toplevel_decoration_v1_destroy(native.decoration);
            if (native.toplevel != nullptr)
                xdg_toplevel_destroy(native.toplevel);
            if (native.shell_surface != nullptr)
                xdg_surface_destroy(native.shell_surface);
            if (native.surface != nullptr)
                wl_surface_destroy(native.surface);

            native.decoration = nullptr;
            native.toplevel = nullptr;
            native.shell_surface = nullptr;
            native.surface = nullptr;
        }

        // ---- events ------------------------------------------------------
        //
        // The compositor describes a change of state in two steps: any number
        // of xdg_toplevel events saying what changes, then one
        // xdg_surface.configure that makes them count. The window answers by
        // acknowledging and drawing itself at the new size.

        void on_toplevel_configure(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height,
                                   wl_array *states)
        {
            auto &native = *static_cast<native_window *>(data);

            native.pending = {};
            native.pending.size = {width, height};

            const auto *first = static_cast<const std::uint32_t *>(states->data);
            const std::size_t count = states->size / sizeof(std::uint32_t);
            for (std::size_t i = 0; i < count; ++i)
            {
                switch (first[i])
                {
                case XDG_TOPLEVEL_STATE_MAXIMIZED: native.pending.maximized = true; break;
                case XDG_TOPLEVEL_STATE_FULLSCREEN: native.pending.fullscreen = true; break;
                case XDG_TOPLEVEL_STATE_ACTIVATED: native.pending.activated = true; break;
                default: break;
                }
            }
        }

        void on_toplevel_close(void *data, xdg_toplevel *)
        {
            emit(static_cast<native_window *>(data)->data->on_close);
        }

        // See pointer_listener in input.cpp for why this is not an initializer list.
        constexpr xdg_toplevel_listener toplevel_listener = [] {
            xdg_toplevel_listener listener{};
            listener.configure = on_toplevel_configure;
            listener.close = on_toplevel_close;
            return listener;
        }();

        void on_surface_configure(void *user_data, xdg_surface *surface, std::uint32_t serial)
        {
            auto &native = *static_cast<native_window *>(user_data);
            window_data &data = *native.data;

            xdg_surface_ack_configure(surface, serial);
            native.configured = true;

            const bool maximized = native.pending.maximized;
            const bool fullscreen = native.pending.fullscreen;
            const bool focused = native.pending.activated;

            // A size of zero leaves the choice to the window.
            vec2i size = native.pending.size;
            if (size.x() <= 0 || size.y() <= 0)
                size = native.windowed_size;
            else if (!maximized && !fullscreen)
                native.windowed_size = size;

            const bool resized = size != data.size;
            const bool was_maximized = std::exchange(data.maximized, maximized);
            const bool was_focused = std::exchange(data.focused, focused);
            data.fullscreen = fullscreen;
            data.size = size;

            if (data.visible)
                present(native);

            // Last, and without touching `native` again: a callback may
            // destroy the window.
            if (resized)
                emit(data.on_resize, size);
            if (maximized != was_maximized)
                emit(data.on_maximize, maximized);
            if (was_maximized && !maximized)
                emit(data.on_restore);
            if (focused != was_focused)
                emit(data.on_focus, focused);
        }

        constexpr xdg_surface_listener surface_listener{
            .configure = on_surface_configure,
        };
    } // namespace
} // namespace platform::wayland

namespace volt::ui
{
    using namespace platform::wayland;

    window::window() = default;

    window::~window()
    {
        destroy();
    }

    std::expected<void, std::string> window::create(const platform::state &platform,
                                                    const window_configuration &config)
    {
        assert(std::this_thread::get_id() == platform.thread && "windows must be created on the platform thread");

        if (m_native)
            return std::unexpected("the window is already open");

        if (config.size.x() <= 0 || config.size.y() <= 0)
            return std::unexpected("a window needs a size");

        // config.position is not used: Wayland windows are placed by the
        // compositor.
        //
        // config.size is logical, and so are a surface's own units, so it is
        // taken as it is and the content scale stays 1. The compositor scales
        // the window for a HiDPI output; drawing it sharp there would mean
        // following the output's scale (wl_surface.preferred_buffer_scale or
        // wp_fractional_scale_v1) with larger buffers, which this backend
        // does not do yet. The scale to report then is that of the buffers
        // over the surface, and get_size() would stay logical.
        m_data.title = config.title;
        m_data.position = {};
        m_data.min_size = config.min_size;
        m_data.size = {std::max(config.size.x(), config.min_size.x()), std::max(config.size.y(), config.min_size.y())};
        m_data.scale = 1.0f;
        m_data.resizable = config.resizable;
        m_data.decorated = config.decorated;
        m_data.fullscreen = config.fullscreen; // requested when the window is shown
        m_data.visible = false;
        m_data.focused = false;
        m_data.maximized = false;
        m_data.minimized = false;

        auto native = std::make_unique<platform::native_window>();
        native->platform = platform;
        native->data = &m_data;
        native->windowed_size = m_data.size;

        // These only fail when the process is out of memory.
        native->surface = wl_compositor_create_surface(platform.compositor);
        if (native->surface != nullptr)
            native->shell_surface = xdg_wm_base_get_xdg_surface(platform.shell, native->surface);
        if (native->shell_surface != nullptr)
            native->toplevel = xdg_surface_get_toplevel(native->shell_surface);

        if (native->toplevel == nullptr)
        {
            release(*native);
            return std::unexpected("could not create the Wayland objects of the window");
        }

        wl_surface_set_user_data(native->surface, native.get());
        xdg_surface_add_listener(native->shell_surface, &surface_listener, native.get());
        xdg_toplevel_add_listener(native->toplevel, &toplevel_listener, native.get());

        if (platform.decorations != nullptr)
        {
            native->decoration =
                zxdg_decoration_manager_v1_get_toplevel_decoration(platform.decorations, native->toplevel);
        }

        // Nothing is sent to the compositor about the window's state yet;
        // show() does that.
        m_native = std::move(native);
        return {};
    }

    void window::destroy()
    {
        if (!m_native)
            return;

        forget_window(m_native->platform.input, *m_native);
        release(*m_native);
        m_native.reset();

        m_data.scale = 1.0f;
        m_data.fullscreen = false;
        m_data.visible = false;
        m_data.focused = false;
        m_data.maximized = false;
        m_data.minimized = false;
    }

    // Mapping a window is a conversation: the window commits its surface
    // without content, the compositor answers with a configure event saying
    // what size and state it should have, and only then may the window attach
    // its first buffer. So the window appears a moment after show() returns,
    // from on_surface_configure.
    void window::show()
    {
        if (!m_native || m_data.visible)
            return;

        m_data.visible = true;
        apply_state(*m_native);
        wl_surface_commit(m_native->surface);
    }

    // Hiding means taking the content away. That returns the window to how it
    // was before it was first shown, as far as the compositor is concerned.
    void window::hide()
    {
        if (!m_native || !m_data.visible)
            return;

        m_data.visible = false;
        m_native->configured = false;
        wl_surface_attach(m_native->surface, nullptr, 0, 0);
        wl_surface_commit(m_native->surface);
    }

    // The compositor decides whether these three happen. If they do, it says
    // so with a configure event, which is what updates is_maximized().
    void window::maximize()
    {
        if (!m_native)
            return;

        show();
        xdg_toplevel_set_maximized(m_native->toplevel);
    }

    void window::minimize()
    {
        if (!m_native)
            return;

        show();
        xdg_toplevel_set_minimized(m_native->toplevel);
    }

    // A minimized window cannot be brought back by its application; only the
    // user can do that.
    void window::restore()
    {
        if (!m_native)
            return;

        show();
        xdg_toplevel_unset_maximized(m_native->toplevel);
    }

    // Wayland does not let a client raise its own window or take the
    // keyboard: the compositor decides, so that windows cannot steal focus.
    // The way to ask is xdg-activation-v1, with a token from an input event
    // the user just made, which this backend does not speak yet. Until it
    // does, all this can do is show a hidden window, which a compositor will
    // usually activate as it maps it.
    void window::focus()
    {
        show();
    }

    void window::set_title(const std::string &title)
    {
        if (!m_native)
            return;

        m_data.title = title;
        xdg_toplevel_set_title(m_native->toplevel, title.c_str());
    }

    void window::set_resizable(bool resizable)
    {
        if (!m_native)
            return;

        m_data.resizable = resizable;
        apply_size_limits(*m_native);
        commit_if_mapped(*m_native);
    }

    // Recorded right away so that it also holds for a window that is still
    // hidden; a configure event corrects it if the compositor refuses.
    void window::set_fullscreen(bool fullscreen)
    {
        if (!m_native)
            return;

        m_data.fullscreen = fullscreen;
        if (fullscreen)
            xdg_toplevel_set_fullscreen(m_native->toplevel, nullptr); // on whichever output the window is on
        else
            xdg_toplevel_unset_fullscreen(m_native->toplevel);
    }

    void window::set_decorated(bool decorated)
    {
        if (!m_native)
            return;

        m_data.decorated = decorated;
        apply_decoration(*m_native);
    }

    display_info window::display(bool)
    {
        // Wayland tells a surface what its output prefers through the
        // color-management protocol, which this backend does not speak yet.
        return {};
    }

    void window::set_position(const math::vec2i &)
    {
        // Not possible on Wayland: the compositor places windows.
    }

    void window::set_size(const math::vec2i &size)
    {
        if (!m_native || size.x() <= 0 || size.y() <= 0)
            return;

        platform::native_window &native = *m_native;
        native.windowed_size = size;
        apply_size_limits(native);

        // While maximized or fullscreen the compositor dictates the size; the
        // new one applies once the window is back to normal.
        if (m_data.maximized || m_data.fullscreen || size == m_data.size)
        {
            commit_if_mapped(native);
            return;
        }

        m_data.size = size;
        if (native.configured && m_data.visible)
            present(native);

        emit(m_data.on_resize, size);
    }

    void window::set_min_size(const math::vec2i &logical)
    {
        m_data.min_size = {std::max(logical.x(), 0), std::max(logical.y(), 0)};
        if (!m_native)
            return;

        // The compositor keeps the window at least this big from the next
        // commit on, and says so with a configure if it has to grow it. A
        // window sized by the application grows here.
        platform::native_window &native = *m_native;
        apply_size_limits(native);
        const math::vec2i grown{std::max(native.windowed_size.x(), m_data.min_size.x()),
                          std::max(native.windowed_size.y(), m_data.min_size.y())};
        if (grown != native.windowed_size)
        {
            set_size(grown);
            return;
        }
        commit_if_mapped(native);
    }

    std::optional<monitor_area> monitor_at(const platform::state &, std::optional<math::vec2i>)
    {
        // Wayland does not let an application place its windows, nor tell
        // it where the outputs are on the desktop: the compositor keeps
        // windows on screen itself.
        return std::nullopt;
    }

} // namespace volt::ui
