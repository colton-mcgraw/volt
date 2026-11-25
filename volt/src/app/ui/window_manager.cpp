#include <app/ui/window_manager.hpp>

#include <app/ui/window_geometry.hpp>
#include <app/ui/windows/menus.hpp>
#include <gpu/context.hpp>
#include <library/project.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace volt::ui
{

    window_manager::~window_manager()
    {
        // Normally both have run by now; these cover an application that
        // failed before its render thread started.
        teardown();
        shutdown();
    }

    void window_manager::create(setup setup)
    {
        m_setup = std::move(setup);

        // The title bars name the project, and mark it while unsaved.
        if (m_setup.services != nullptr)
            m_project_connection = m_setup.services->project_changed.connect([this] { reconfigure(); });
    }

    void window_manager::set_contents(std::array<std::unique_ptr<window_content>, window_kind_count> contents)
    {
        m_contents = std::move(contents);
    }

    // ---- platform thread --------------------------------------------------------------------

    std::expected<platform::native_window *, std::string> window_manager::create_first(window_kind kind)
    {
        // Before the render thread exists this thread is the interface
        // thread too, so it asks the book itself.
        const window_book::open_result opened = m_book.request_open(kind);
        auto w = std::make_shared<app_window>(kind, opened.serial);
        if (auto made = make_window(*w, describe(platform_task::kind::create, kind, w)); !made)
        {
            m_book.failed(kind, opened.serial);
            return std::unexpected(std::move(made).error());
        }
        m_slots[index_of(kind)] = w;
        m_first = w;
        return w->window.native();
    }

    std::expected<void, std::string> window_manager::finish_first()
    {
        if (!m_first)
            return std::unexpected(std::string("there is no first window"));
        std::shared_ptr<app_window> w = std::exchange(m_first, {});
        if (auto connected = connect_window(w); !connected)
            return connected;
        w->window.show();
        to_interface({.what = interface_message::kind::created, .window = w});
        return {};
    }

    void window_manager::inject_drop(window_kind kind, std::span<const std::string> paths)
    {
        for (const auto &w : m_platform_windows)
        {
            if (w->kind == kind && w->window.is_open())
            {
                w->input.processor().on_drop(paths);
                return;
            }
        }
    }

    std::expected<void, std::string> window_manager::make_window(app_window &w, const platform_task &task)
    {
        window_configuration config;
        config.title = task.title;
        config.size = task.default_size;
        config.min_size = task.min_size;
        if (const std::optional<geometry> &last = m_geometry[index_of(w.kind)])
        {
            // Where it was; the platform keeps it inside the work area of
            // whatever monitor that is now.
            config.position = last->position;
            config.size = last->size;
        }
        else
        {
            // In the middle of the project manager's monitor, or of the
            // primary one (where the project manager itself opens), each
            // kind a step aside: the same place at every launch.
            std::optional<math::vec2i> anchor;
            for (const std::shared_ptr<app_window> &other : m_platform_windows)
            {
                if (other.get() != &w && other->kind == window_kind::project_manager && other->window.is_open())
                {
                    const math::vec2i size = other->window.get_size();
                    anchor = other->window.get_position() + math::vec2i{size.x() / 2, size.y() / 2};
                }
            }
            if (const std::optional<monitor_area> area = monitor_at(m_setup.platform, anchor))
                config.position = default_position(*area, config.size, w.kind);
        }
        return w.window.create(m_setup.platform, config);
    }

    std::expected<void, std::string> window_manager::connect_window(const std::shared_ptr<app_window> &shared)
    {
        app_window &w = *shared;

        // The surface only; the swapchain is built by the render thread at
        // its first acquire.
        if (auto created = w.swapchain.create(*m_setup.gpu, *w.window.native()); !created)
        {
            w.window.destroy();
            return created;
        }

        w.size.store(w.window.get_size());
        w.display.store(w.window.display());
        w.initial_scale = w.window.content_scale();

        // The interface lives on the render thread and gets the input
        // there, queued; the render thread drains it at its wake-ups.
        w.input.enable_queue(m_setup.wake);

        // What has to happen here, on the platform thread, listens on the
        // immediate bus. The listeners hold the window by reference: they
        // are removed (retire()) before it goes.
        events::bus &bus = w.input.immediate();
        app_window *window = &w;
        w.platform_tokens.emplace_back(bus.add_listener<close_request_event>([this, window](const close_request_event &) {
            // The interface decides; see the protocol in the header.
            to_interface({.what = interface_message::kind::close_requested,
                          .about = window->kind,
                          .serial = window->serial});
        }));
        w.platform_tokens.emplace_back(bus.add_listener<window_resize_event>([this, window](const window_resize_event &e) {
            window->size.store(e.size);
            update_display(*window, false);
            window->redraw.store(true);
            m_setup.wake();
        }));

        // The two ways a window's display changes under it: it is moved to
        // another monitor, or the monitor's mode is changed. Nothing reports
        // the second, but the user has to leave the window to do it in the
        // system settings, so coming back is when to look.
        w.platform_tokens.emplace_back(bus.add_listener<window_move_event>(
            [this, window](const window_move_event &) { update_display(*window, false); }));
        w.platform_tokens.emplace_back(bus.add_listener<focus_event>([this, window](const focus_event &e) {
            if (e.focused)
                update_display(*window, true);
        }));

        // Minimizing reports no resize (the window keeps the size it comes
        // back with); the surface then has no area, which acquire() finds by
        // itself. Coming back asks for a frame.
        w.platform_tokens.emplace_back(bus.add_listener<window_state_event>([this, window](const window_state_event &) {
            window->redraw.store(true);
            m_setup.wake();
        }));

        // From here on the window's input arrives as events, its scale first.
        w.input.attach(w.window);
        m_platform_windows.push_back(shared);
        return {};
    }

    void window_manager::update_display(app_window &w, bool refresh)
    {
        const display_info display = w.window.display(refresh);
        if (w.display.exchange(display) != display)
        {
            w.redraw.store(true);
            m_setup.wake();
        }
    }

    void window_manager::run_platform_tasks()
    {
        std::vector<platform_task> tasks;
        {
            std::lock_guard lock(m_platform_mutex);
            tasks.swap(m_platform_tasks);
        }

        for (platform_task &task : tasks)
        {
            app_window &w = *task.window;
            switch (task.what)
            {
            case platform_task::kind::create: {
                auto made = make_window(w, task);
                if (made)
                    made = connect_window(task.window);
                if (!made)
                {
                    to_interface({.what = interface_message::kind::failed,
                                  .window = task.window,
                                  .error = std::move(made).error()});
                    break;
                }
                // Shown, in front, with the keyboard: the user asked for it.
                w.window.focus();
                to_interface({.what = interface_message::kind::created, .window = task.window});
                break;
            }
            case platform_task::kind::focus:
                w.window.focus();
                break;
            case platform_task::kind::retire:
                retire(w);
                break;
            case platform_task::kind::configure:
                // Closed windows ignore both.
                w.window.set_title(task.title);
                w.window.set_min_size(task.min_size);
                break;
            }
        }
    }

    void window_manager::retire(app_window &w)
    {
        // Where it was, logical size, for the next window of its kind; not
        // while minimized or maximized, whose sizes are not the user's.
        if (w.window.is_open() && !w.window.is_minimized() && !w.window.is_maximized())
        {
            const float scale = w.window.content_scale() > 0.0f ? w.window.content_scale() : 1.0f;
            const math::vec2i pixels = w.window.get_size();
            geometry g;
            g.position = w.window.get_position();
            g.size = {static_cast<int>(std::lround(static_cast<float>(pixels.x()) / scale)),
                      static_cast<int>(std::lround(static_cast<float>(pixels.y()) / scale))};
            if (g.size.x() > 0 && g.size.y() > 0)
                m_geometry[index_of(w.kind)] = g;
        }

        // The listeners, then the input (which takes its callbacks out of
        // the window), the surface, the window.
        w.platform_tokens.clear();
        w.input.detach();
        w.swapchain.destroy();
        w.window.destroy();
        std::erase_if(m_platform_windows, [&](const std::shared_ptr<app_window> &p) { return p.get() == &w; });
    }

    void window_manager::shutdown()
    {
        // The render thread has ended: nothing will run the tasks, nor read
        // the messages.
        {
            std::lock_guard lock(m_platform_mutex);
            m_platform_tasks.clear();
        }
        {
            std::lock_guard lock(m_interface_mutex);
            m_interface_messages.clear();
        }
        m_first.reset();

        // No frames run any more, so a swapchain still built is released by
        // its destroy().
        while (!m_platform_windows.empty())
        {
            std::shared_ptr<app_window> w = m_platform_windows.back();
            retire(*w);
        }
    }

    void window_manager::to_interface(interface_message message)
    {
        {
            std::lock_guard lock(m_interface_mutex);
            m_interface_messages.push_back(std::move(message));
        }
        if (m_setup.wake)
            m_setup.wake();
    }

    // ---- render (interface) thread -----------------------------------------------------------

    void window_manager::to_platform(platform_task task)
    {
        {
            std::lock_guard lock(m_platform_mutex);
            m_platform_tasks.push_back(std::move(task));
        }
        platform::wake(m_setup.platform);
    }

    void window_manager::process()
    {
        std::vector<interface_message> messages;
        {
            std::lock_guard lock(m_interface_mutex);
            messages.swap(m_interface_messages);
        }

        for (interface_message &message : messages)
        {
            switch (message.what)
            {
            case interface_message::kind::created:
                on_created(message.window);
                break;
            case interface_message::kind::failed: {
                const window_kind kind = message.window->kind;
                m_book.failed(kind, message.window->serial);
                if (m_slots[index_of(kind)] == message.window)
                    m_slots[index_of(kind)].reset();
                std::cerr << "Cannot open the " << window_name(kind) << ": " << message.error << std::endl;
                if (m_setup.services != nullptr)
                    m_setup.services->show_message(window_kind::project_manager,
                                                   "Cannot open the " + std::string(window_name(kind)) + ": " +
                                                       message.error,
                                                   std::chrono::seconds(10), log_level::error);
                if (kind == window_kind::project_manager)
                    exit();
                break;
            }
            case interface_message::kind::close_requested:
                on_close_requested(message.about, message.serial);
                break;
            }
        }
    }

    void window_manager::on_created(const std::shared_ptr<app_window> &w)
    {
        if (!m_book.created(w->kind, w->serial))
        {
            // Closed while it was being made, or the application is
            // exiting. Its swapchain was never acquired.
            to_platform({.what = platform_task::kind::retire, .window = w});
            return;
        }

        build_interface(*w);
        m_live.push_back(w);
    }

    void window_manager::build_interface(app_window &w)
    {
        app_services &services = *m_setup.services;
        window_content &content = *m_contents[index_of(w.kind)];

        w.screen = std::make_unique<ui::screen>(services.text(), services.theme());

        // Copy and paste with other applications, in every window.
        w.screen->set_clipboard(&services.system_clipboard());

        // The platform cannot set the cursor yet: this is where it would.
        w.screen->set_cursor_callback([](cursor_shape) {});

        // The screen asks for frames through the render thread's wake;
        // whether it wants one, the render loop asks it (needs_redraw()).
        w.screen->set_redraw_callback(m_setup.wake);

        w.host = std::make_unique<window_host>(w.kind, *w.screen, services);
        // Made on the platform thread before the window was handed over,
        // and destroyed there only after it is retired, which is after the
        // host is gone.
        w.host->set_native(w.window.native());
        w.host->apply_scale(w.initial_scale);
        w.screen->set_size(w.size.load());

        w.content = &content;
        content.set_host(w.host.get());
        add_standard_commands(*w.host);
        window_layout layout = content.build(*w.host);

        // The frame around the content (<app/ui/windows/content.hpp>).
        auto root = std::make_unique<panel>("", surface::background, 0.0f, insets{});
        root->set_name("root");
        root->add(build_menu_bar(*w.host, layout));
        if (layout.body)
        {
            layout.body->layout().flex = 1.0f;
            root->add(std::move(layout.body));
        }
        else
        {
            root->emplace<spacer>().layout().flex = 1.0f;
        }
        root->add(w.host->status().build(layout.status));
        w.screen->set_root(std::move(root));

        // Its input, from the queue. Events that arrived while it was being
        // made are still waiting there, and come with the next drain.
        w.ui_tokens = w.screen->connect(w.input.queued());
        app_window *window = &w;
        w.ui_tokens.emplace_back(w.input.queued().add_listener<drop_event>([window](const drop_event &e) {
            window->content->drop(e.paths, e.position);
        }));

        // A new monitor's scale comes before the resize it brings, so the
        // layout on the resize has it.
        w.ui_tokens.emplace_back(w.input.queued().add_listener<window_scale_event>([window](const window_scale_event &e) {
            window->host->apply_scale(e.scale);
            window->content->scale_changed(window->host->scale());
        }));
    }

    void window_manager::on_close_requested(window_kind kind, std::uint64_t serial)
    {
        if (m_book.serial_of(kind) != serial || m_book.state_of(kind) == window_book::state::closed)
            return; // about a window that has gone already
        close_window(kind);
    }

    void window_manager::open_window(window_kind kind)
    {
        const window_book::open_result result = m_book.request_open(kind);
        switch (result.action)
        {
        case window_book::open_action::none:
            break;
        case window_book::open_action::create: {
            auto w = std::make_shared<app_window>(kind, result.serial);
            m_slots[index_of(kind)] = w;
            to_platform(describe(platform_task::kind::create, kind, std::move(w)));
            break;
        }
        case window_book::open_action::focus:
            if (m_slots[index_of(kind)])
                to_platform({.what = platform_task::kind::focus, .window = m_slots[index_of(kind)]});
            break;
        }
    }

    void window_manager::close_window(window_kind kind)
    {
        // The project manager is the application. Exiting goes through the
        // services, which ask first about unsaved project changes.
        if (kind == window_kind::project_manager)
        {
            if (m_setup.services != nullptr)
                m_setup.services->exit();
            else
                exit();
            return;
        }

        // What the window holds that closing it would lose is asked about
        // first (its close guards); it closes once they let it go, unless it
        // has gone meanwhile, or another window of its kind has taken its
        // place. A window still being made has nothing to ask.
        const std::shared_ptr<app_window> &w = m_slots[index_of(kind)];
        if (!m_book.is_open(kind) || !w || !w->host)
        {
            set_aside(kind);
            return;
        }
        const std::uint64_t serial = w->serial;
        w->host->guards().confirm([this, kind, serial] {
            if (m_book.serial_of(kind) == serial && m_book.is_open(kind))
                set_aside(kind);
        });
    }

    void window_manager::set_aside(window_kind kind)
    {
        const window_book::close_action action = m_book.request_close(kind);
        std::shared_ptr<app_window> w = std::move(m_slots[index_of(kind)]);
        m_slots[index_of(kind)].reset();
        switch (action)
        {
        case window_book::close_action::none:
            break;
        case window_book::close_action::forget:
            // Still being made: on_created() retires it when it comes.
            break;
        case window_book::close_action::tear_down:
            if (w)
            {
                std::erase(m_live, w);
                m_closing.push_back(std::move(w));
            }
            break;
        }
    }

    bool window_manager::is_open(window_kind kind) const
    {
        return m_book.is_open(kind);
    }

    window_host *window_manager::host(window_kind kind)
    {
        if (!m_book.is_open(kind))
            return nullptr;
        const std::shared_ptr<app_window> &w = m_slots[index_of(kind)];
        return w ? w->host.get() : nullptr;
    }

    void window_manager::exit()
    {
        m_book.begin_exit();
        if (m_setup.exit)
            m_setup.exit();
    }

    void window_manager::settings_changed()
    {
        app_services &services = *m_setup.services;
        for (const std::shared_ptr<app_window> &w : m_live)
        {
            w->screen->set_theme(services.theme());
            w->host->apply_scale(w->host->content_scale());
            w->content->theme_changed();
            w->content->scale_changed(w->host->scale());

            // Whatever changed, the window shows it: a setting the render
            // loop reads (the color output) changes nothing in the screen
            // that would ask for a frame.
            w->screen->request_redraw();
            w->redraw.store(true);
        }

        // The minimums follow the user's scale.
        reconfigure();
    }

    window_manager::platform_task window_manager::describe(platform_task::kind what, window_kind kind,
                                                           std::shared_ptr<app_window> w) const
    {
        platform_task task{.what = what, .window = std::move(w)};
        const app_services *services = m_setup.services;
        const float user = services != nullptr ? services->user_scale() : 1.0f;
        if (const window_content *content = m_contents[index_of(kind)].get())
        {
            task.default_size = with_user_scale(content->default_size(), user);
            task.min_size = with_user_scale(content->min_size(), user);
        }
        else
        {
            task.default_size = {1280, 800};
        }

        std::string project;
        bool modified = false;
        if (services != nullptr && services->project() != nullptr)
        {
            project = services->project()->name;
            if (project.empty())
                project = library::path_to_utf8(services->project_path().stem());
            modified = services->project_modified();
        }
        task.title = window_title(kind, project, modified);
        return task;
    }

    void window_manager::reconfigure()
    {
        // Opening ones too: their create task is ahead of this one.
        for (const std::shared_ptr<app_window> &w : m_slots)
        {
            if (w)
                to_platform(describe(platform_task::kind::configure, w->kind, w));
        }
    }

    void window_manager::drain()
    {
        // Over a copy: a handler may close a window, which takes it out of
        // m_live (it is taken down later, in collect()).
        const std::vector<std::shared_ptr<app_window>> live = m_live;
        for (const std::shared_ptr<app_window> &w : live)
        {
            if (w->screen)
                w->input.drain();
        }
    }

    void window_manager::update(time_point now)
    {
        const std::vector<std::shared_ptr<app_window>> live = m_live;
        for (const std::shared_ptr<app_window> &w : live)
        {
            if (!w->screen)
                continue;
            w->screen->update(now);
            w->content->update(now);
        }
    }

    void window_manager::take_down(app_window &w)
    {
        // Popups first, while everything they may call in their `closed`
        // still exists; then the content forgets its widgets; then the
        // listeners on the queued bus, the host, the screen.
        if (w.screen)
            w.screen->close_all_popups();
        if (w.content != nullptr)
        {
            w.content->detach();
            w.content->set_host(nullptr);
            w.content = nullptr;
        }
        w.ui_tokens.clear();
        w.host.reset();
        w.screen.reset();
        w.list.clear();

        // Between frames: <gpu/swapchain.hpp>, "Removing a window", step 2.
        w.swapchain.release();
    }

    void window_manager::collect()
    {
        // take_down() may run handlers (popups' `closed`) that close more.
        bool closed = false;
        while (!m_closing.empty())
        {
            std::shared_ptr<app_window> w = std::move(m_closing.back());
            m_closing.pop_back();
            take_down(*w);
            to_platform({.what = platform_task::kind::retire, .window = std::move(w)});
            closed = true;
        }

        // The logo sizes only the closed windows showed.
        if (closed && m_setup.services != nullptr)
            m_setup.services->trim_logos();
    }

    std::optional<time_point> window_manager::next_wakeup() const
    {
        std::optional<time_point> next;
        for (const std::shared_ptr<app_window> &w : m_live)
        {
            if (!w->screen)
                continue;
            if (const auto due = w->screen->next_wakeup(); due && (!next || *due < *next))
                next = due;
        }
        return next;
    }

    void window_manager::teardown()
    {
        if (m_torn_down)
            return;
        m_torn_down = true;
        m_book.begin_exit();
        if (m_setup.services != nullptr && m_project_connection != 0)
            m_setup.services->project_changed.disconnect(m_project_connection);
        m_project_connection = 0;

        for (const std::shared_ptr<app_window> &w : m_live)
            take_down(*w);
        for (const std::shared_ptr<app_window> &w : m_closing)
            take_down(*w);
        m_live.clear();
        m_closing.clear();
        m_slots = {};

        // The platform thread destroys the windows once this thread has
        // ended (shutdown()); the contents go now, on the interface thread.
        for (std::unique_ptr<window_content> &content : m_contents)
            content.reset();
    }

} // namespace volt::ui
