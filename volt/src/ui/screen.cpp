#include <ui/screen.hpp>

#include <ui/painter.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        // The tooltip the screen shows by itself: a line of text in a box.
        class tooltip_box final : public widget
        {
        public:
            explicit tooltip_box(std::string text) : m_text(std::move(text)) { set_hit_testable(false); }

        protected:
            math::vec2f on_measure(const constraints &) override
            {
                const ui::theme &t = theme();
                const text_style style = t.small_style();
                font::layout_options options;
                options.max_width = 360.0f;
                const font::text_layout laid = fonts().layout(m_text, style, options);
                // The widest line: with a width limit set, the extent is the limit.
                float widest = 0.0f;
                for (const font::text_line &line : laid.lines)
                    widest = std::max(widest, line.width);
                return {std::ceil(widest) + 2.0f * 7.0f, std::ceil(laid.extent.y()) + 2.0f * 4.0f};
            }

            void on_paint(painter &p) override
            {
                const ui::theme &t = theme();
                p.frame(bounds(), t.tooltip, t.border, t.small_radius);
                font::layout_options options;
                options.max_width = 360.0f;
                const font::text_layout laid = fonts().layout(m_text, t.small_style(), options);
                p.layout(laid, t.small_style(), {bounds().x + 7.0f, bounds().y + 4.0f}, t.text);
            }

        private:
            std::string m_text;
        };

        constexpr auto animation_frame = std::chrono::milliseconds(16);
    } // namespace

    screen::screen(text_engine &text, ui::theme theme) : m_text(text), m_theme(std::move(theme))
    {
    }

    screen::~screen()
    {
        // Popups and the root go while the screen is still whole, since
        // they tell it as they go.
        m_graveyard.clear();
        while (!m_popups.empty())
        {
            popup_entry entry = std::move(m_popups.back());
            m_popups.pop_back();
            entry.content->detach();
        }
        if (m_root)
            m_root->detach();
        m_root.reset();
    }

    // ---- the tree ---------------------------------------------------------------

    void screen::set_root(std::unique_ptr<widget> root)
    {
        if (m_root)
        {
            m_root->detach();
            m_graveyard.push_back(std::move(m_root));
        }
        m_root = std::move(root);
        if (m_root)
            m_root->attach(this);
        invalidate_layout();
    }

    void screen::set_theme(ui::theme theme)
    {
        m_theme = std::move(theme);
        invalidate_layout();
    }

    void screen::set_clipboard(ui::clipboard *clipboard) noexcept
    {
        m_clipboard = clipboard != nullptr ? clipboard : &m_default_clipboard;
    }

    void screen::set_size(const math::vec2i &pixels)
    {
        if (pixels == m_pixels)
            return;
        m_pixels = pixels;
        m_size = {static_cast<float>(pixels.x()) / m_scale, static_cast<float>(pixels.y()) / m_scale};
        invalidate_layout();
    }

    void screen::set_scale(float scale)
    {
        if (!(scale > 0.0f) || scale == m_scale)
            return;
        m_scale = scale;
        m_size = {static_cast<float>(m_pixels.x()) / m_scale, static_cast<float>(m_pixels.y()) / m_scale};
        hide_tooltip();
        invalidate_layout();
    }

    // ---- frames -----------------------------------------------------------------

    void screen::set_redraw_callback(std::function<void()> callback)
    {
        m_redraw_callback = std::move(callback);
    }

    void screen::request_redraw()
    {
        if (m_needs_redraw)
            return;
        m_needs_redraw = true;
        if (m_redraw_callback)
            m_redraw_callback();
    }

    void screen::invalidate_layout() noexcept
    {
        m_layout_dirty = true;
        request_redraw();
    }

    void screen::update(time_point now)
    {
        m_now = now;

        std::vector<std::function<void()>> posted;
        {
            std::lock_guard lock(m_posted_mutex);
            posted.swap(m_posted);
        }
        for (auto &task : posted)
            task();

        // Due timers, in the order they are due. A timer may schedule
        // another, which waits for the next update.
        std::vector<timer> due;
        for (auto it = m_timers.begin(); it != m_timers.end();)
        {
            if (it->when <= now)
            {
                due.push_back(std::move(*it));
                it = m_timers.erase(it);
            }
            else
            {
                ++it;
            }
        }
        std::ranges::sort(due, [](const timer &a, const timer &b) { return a.when < b.when; });
        for (timer &t : due)
            t.callback();

        if (!m_animating.empty())
        {
            const float seconds =
                std::clamp(std::chrono::duration<float>(now - m_last_animation).count(), 0.0f, 0.1f);
            m_last_animation = now;

            // Each step may stop its own animation, or start another's.
            const std::vector<widget *> running = m_animating;
            for (widget *w : running)
            {
                if (std::ranges::find(m_animating, w) == m_animating.end())
                    continue;
                if (!w->on_animate(seconds))
                    std::erase(m_animating, w);
            }
            request_redraw();
        }

        bury();
    }

    std::optional<time_point> screen::next_wakeup() const
    {
        std::optional<time_point> next;
        for (const timer &t : m_timers)
        {
            if (!next || t.when < *next)
                next = t.when;
        }
        if (!m_animating.empty())
        {
            const time_point frame = m_now + animation_frame;
            if (!next || frame < *next)
                next = frame;
        }
        return next;
    }

    void screen::start_animation(widget &target)
    {
        if (std::ranges::find(m_animating, &target) != m_animating.end())
            return;
        if (m_animating.empty())
            m_last_animation = m_now;
        m_animating.push_back(&target);
        request_redraw();
    }

    void screen::arrange_popup(popup_entry &entry)
    {
        widget &content = *entry.content;
        const popup_options &o = entry.options;

        math::vec2f size = content.measure(constraints::loose(m_size));
        if (o.match_anchor_width && size.x() < o.anchor.width)
            size[0] = o.anchor.width;
        size[0] = std::min(size.x(), m_size.x());
        size[1] = std::min(size.y(), m_size.y());

        math::vec2f at{};
        switch (o.placement)
        {
        case placement::below:
            at = {o.anchor.x, o.anchor.bottom()};
            if (at.y() + size.y() > m_size.y() && o.anchor.y - size.y() >= 0.0f)
                at[1] = o.anchor.y - size.y();
            break;
        case placement::right:
            at = {o.anchor.right(), o.anchor.y};
            if (at.x() + size.x() > m_size.x())
                at[0] = o.anchor.x - size.x();
            break;
        case placement::point:
            at = {o.anchor.x, o.anchor.y};
            if (at.y() + size.y() > m_size.y() && o.anchor.y - size.y() >= 0.0f)
                at[1] = o.anchor.y - size.y();
            break;
        case placement::centered:
            at = (m_size - size) * 0.5f;
            break;
        }

        at[0] = std::clamp(at.x(), 0.0f, std::max(0.0f, m_size.x() - size.x()));
        at[1] = std::clamp(at.y(), 0.0f, std::max(0.0f, m_size.y() - size.y()));
        content.arrange({std::round(at.x()), std::round(at.y()), size.x(), size.y()});
    }

    void screen::layout()
    {
        if (!m_layout_dirty)
            return;
        m_layout_dirty = false;

        if (m_root)
        {
            m_root->measure(constraints::tight(m_size));
            m_root->arrange({0.0f, 0.0f, m_size.x(), m_size.y()});
        }
        for (popup_entry &entry : m_popups)
            arrange_popup(entry);

        // Things moved under a pointer that did not.
        if (m_pointer_inside && !m_capture.captured())
            update_hover(hit_test(m_pointer));
    }

    void screen::paint(draw_list &list)
    {
        layout();

        const auto paint_all = [&] {
            list.clear();
            painter p(list, m_text, m_theme, m_scale);
            if (m_root)
                m_root->paint(p);

            for (popup_entry &entry : m_popups)
            {
                if (entry.options.modal)
                    p.fill({0.0f, 0.0f, m_size.x(), m_size.y()}, m_theme.backdrop);
                const rect &b = entry.content->bounds();
                p.shadow(b.translated({0.0f, 2.0f}), m_theme.radius, m_theme.shadow_blur * 0.5f, m_theme.shadow);
                entry.content->paint(p);
            }
        };

        paint_all();
        if (m_text.overflowed())
        {
            m_text.reset();
            paint_all();
        }
        m_needs_redraw = false;
    }

    // ---- time -------------------------------------------------------------------

    screen::timer_id screen::schedule(time_point when, std::function<void()> callback)
    {
        m_timers.push_back({++m_last_timer, when, std::move(callback)});
        if (m_redraw_callback)
            m_redraw_callback(); // the application recomputes when to wake
        return m_last_timer;
    }

    void screen::cancel(timer_id id) noexcept
    {
        std::erase_if(m_timers, [id](const timer &t) { return t.id == id; });
    }

    void screen::post(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_posted_mutex);
            m_posted.push_back(std::move(task));
        }
        if (m_redraw_callback)
            m_redraw_callback();
    }

    // ---- helpers ----------------------------------------------------------------

    math::vec2f screen::logical(const math::vec2i &pixels) const noexcept
    {
        return {static_cast<float>(pixels.x()) / m_scale, static_cast<float>(pixels.y()) / m_scale};
    }

    pointer_event screen::pointer_from(const math::vec2f &position, modifiers mods) const noexcept
    {
        pointer_event e;
        e.position = position;
        e.mods = mods;
        return e;
    }

    std::size_t screen::first_reachable_popup() const noexcept
    {
        for (std::size_t i = m_popups.size(); i-- > 0;)
        {
            if (m_popups[i].options.modal)
                return i;
        }
        return 0;
    }

    int screen::popup_index_of(const widget *w) const noexcept
    {
        if (w == nullptr)
            return -1;
        for (std::size_t i = 0; i < m_popups.size(); ++i)
        {
            const widget *content = m_popups[i].content.get();
            if (content == w || content->is_ancestor_of(*w))
                return static_cast<int>(i);
        }
        return -1;
    }

    widget *screen::hit_test(const math::vec2f &point) const
    {
        const std::size_t first = first_reachable_popup();
        for (std::size_t i = m_popups.size(); i-- > first;)
        {
            const popup_entry &entry = m_popups[i];
            if (!entry.options.hit_testable)
                continue;
            if (widget *hit = entry.content->hit_test(point))
                return hit;
        }

        const bool modal = !m_popups.empty() && m_popups[first].options.modal;
        if (modal || !m_root)
            return nullptr;
        return m_root->hit_test(point);
    }

    template <typename F>
    widget *screen::bubble(widget *target, F &&handler)
    {
        for (widget *w = target; w != nullptr; w = w->parent())
        {
            if (w->is_enabled() && handler(w))
                return w;
        }
        return nullptr;
    }

    void screen::update_hover(widget *target)
    {
        std::vector<widget *> chain;
        for (widget *w = target; w != nullptr; w = w->parent())
            chain.push_back(w);
        std::ranges::reverse(chain);

        if (chain == m_hover)
            return;

        const widget *old_deepest = hovered();
        const std::vector<widget *> old = std::exchange(m_hover, chain);

        for (auto it = old.rbegin(); it != old.rend(); ++it)
        {
            if (std::ranges::find(m_hover, *it) == m_hover.end())
            {
                (*it)->m_hovered = false;
                (*it)->on_pointer_leave();
            }
        }
        for (widget *w : m_hover)
        {
            if (std::ranges::find(old, w) == old.end())
            {
                w->m_hovered = true;
                w->on_pointer_enter();
            }
        }

        if (hovered() != old_deepest)
        {
            // A new widget under the pointer: the tooltip starts over.
            hide_tooltip();
            for (widget *w = hovered(); w != nullptr; w = w->parent())
            {
                if (!w->tooltip().empty())
                {
                    m_tooltip_for = w;
                    const auto delay = std::chrono::duration<float>(m_theme.tooltip_delay);
                    m_tooltip_timer = schedule_after(std::chrono::duration_cast<clock::duration>(delay),
                                                     [this] { show_tooltip(); });
                    break;
                }
            }
        }
    }

    void screen::update_cursor()
    {
        widget *target = captured();
        if (target == nullptr)
            target = hovered();

        cursor_shape shape = cursor_shape::arrow;
        for (widget *w = target; w != nullptr; w = w->parent())
        {
            shape = w->cursor(m_pointer);
            if (shape != cursor_shape::arrow)
                break;
        }

        if (shape != m_cursor)
        {
            m_cursor = shape;
            if (m_cursor_callback)
                m_cursor_callback(shape);
        }
    }

    void screen::hide_tooltip()
    {
        if (m_tooltip_timer != 0)
            cancel(m_tooltip_timer);
        m_tooltip_timer = 0;
        m_tooltip_for = nullptr;
        if (m_tooltip != nullptr)
        {
            widget *shown = std::exchange(m_tooltip, nullptr);
            close_popup(*shown);
        }
    }

    void screen::show_tooltip()
    {
        m_tooltip_timer = 0;
        if (m_tooltip_for == nullptr || m_capture.captured())
            return;

        popup_options options;
        options.anchor = {m_pointer.x() + 2.0f, m_pointer.y() + 20.0f, 0.0f, 0.0f};
        options.placement = placement::point;
        options.take_focus = false;
        options.hit_testable = false;
        options.dismissable = false;
        m_tooltip = &open_popup(std::make_unique<tooltip_box>(m_tooltip_for->tooltip()), std::move(options));
    }

    void screen::bury()
    {
        m_graveyard.clear();
    }

    // ---- focus --------------------------------------------------------------------

    void screen::set_focus(widget *target, focus_reason reason)
    {
        if (target != nullptr && target->owner() != this)
            target = nullptr;

        const bool visible = target != nullptr && reason == focus_reason::keyboard;
        if (target == m_focus)
        {
            if (visible != m_focus_visible && target != nullptr)
            {
                m_focus_visible = visible;
                request_redraw();
            }
            return;
        }

        widget *old = std::exchange(m_focus, target);
        m_focus_visible = visible;
        if (old != nullptr)
            old->on_focus_changed(false, reason);
        if (target != nullptr && m_focus == target)
            target->on_focus_changed(true, reason);
        request_redraw();
    }

    bool screen::focus_next(bool backwards)
    {
        // The scope: the topmost modal popup, or the popup holding the
        // focus, or the tree.
        widget *scope = m_root.get();
        if (!m_popups.empty())
        {
            const std::size_t first = first_reachable_popup();
            if (m_popups[first].options.modal)
                scope = m_popups[first].content.get();
            const int holding = popup_index_of(m_focus);
            if (holding >= 0)
                scope = m_popups[static_cast<std::size_t>(holding)].content.get();
        }
        if (scope == nullptr)
            return false;

        std::vector<widget *> order;
        const auto collect = [&](auto &&self, widget &w) -> void {
            if (!w.visible() || !w.enabled())
                return;
            if (w.focusable())
                order.push_back(&w);
            for (const auto &child : w.children())
                self(self, *child);
        };
        collect(collect, *scope);
        if (order.empty())
            return false;

        const auto current = std::ranges::find(order, m_focus);
        std::size_t next = 0;
        if (current == order.end())
        {
            next = backwards ? order.size() - 1 : 0;
        }
        else
        {
            const auto index = static_cast<std::size_t>(current - order.begin());
            next = backwards ? (index + order.size() - 1) % order.size() : (index + 1) % order.size();
        }
        set_focus(order[next], focus_reason::keyboard);
        return true;
    }

    // ---- pointer --------------------------------------------------------------------

    widget *screen::captured() const noexcept
    {
        return m_capture.captured() ? *m_capture.captured() : nullptr;
    }

    void screen::capture(widget &target)
    {
        m_capture.transfer(&target);
    }

    void screen::release_capture() noexcept
    {
        m_capture.reset();
    }

    // ---- popups ---------------------------------------------------------------------

    widget &screen::open_popup(std::unique_ptr<widget> content, popup_options options)
    {
        popup_entry entry;
        entry.previous_focus = m_focus;
        entry.options = std::move(options);
        entry.content = std::move(content);
        widget &shown = *entry.content;
        const bool take_focus = entry.options.take_focus;

        m_popups.push_back(std::move(entry));
        shown.attach(this);
        arrange_popup(m_popups.back());
        invalidate_layout();

        if (take_focus)
        {
            widget *target = nullptr;
            const auto first = [&](auto &&self, widget &w) -> void {
                if (target != nullptr || !w.visible() || !w.enabled())
                    return;
                if (w.focusable())
                    target = &w;
                for (const auto &child : w.children())
                    self(self, *child);
            };
            first(first, shown);
            if (target != nullptr)
                set_focus(target, focus_reason::programmatic);
        }
        return shown;
    }

    void screen::close_popups_from(std::size_t index)
    {
        while (m_popups.size() > index)
        {
            popup_entry &top = m_popups.back();
            widget &content = *top.content;

            // Focus leaves before the widgets do, so whoever had it is told.
            if (m_focus != nullptr && (m_focus == &content || content.is_ancestor_of(*m_focus)))
            {
                widget *back = top.previous_focus;
                set_focus(back != nullptr && back->owner() == this ? back : nullptr);
            }
            if (m_tooltip == &content)
                m_tooltip = nullptr;

            popup_entry entry = std::move(top);
            m_popups.pop_back();
            entry.content->detach();
            if (entry.options.closed)
                entry.options.closed();
            m_graveyard.push_back(std::move(entry.content));
        }
        if (m_pointer_inside && !m_capture.captured())
            update_hover(hit_test(m_pointer));
        request_redraw();
    }

    void screen::close_popup(widget &content)
    {
        const int index = popup_index_of(&content);
        if (index >= 0)
            close_popups_from(static_cast<std::size_t>(index));
    }

    void screen::close_all_popups()
    {
        m_tooltip = nullptr;
        close_popups_from(0);
    }

    bool screen::is_open(const widget &content) const noexcept
    {
        return std::ranges::any_of(m_popups, [&](const popup_entry &e) { return e.content.get() == &content; });
    }

    // ---- commands ---------------------------------------------------------------------

    void screen::add_command(std::string id, std::function<void()> action)
    {
        m_commands[std::move(id)] = std::move(action);
    }

    bool screen::run_command(std::string_view id)
    {
        const auto found = m_commands.find(id);
        if (found == m_commands.end() || !found->second)
            return false;
        const auto action = found->second; // it may replace itself
        action();
        return true;
    }

    bool screen::has_command(std::string_view id) const
    {
        return m_commands.find(id) != m_commands.end();
    }

    std::string screen::shortcut_text(std::string_view command) const
    {
        const auto bound = m_shortcuts.shortcuts_for(command);
        return bound.empty() ? std::string{} : to_string(bound.front());
    }

    // ---- forgetting ----------------------------------------------------------------------

    void screen::forget(widget &gone) noexcept
    {
        std::erase(m_hover, &gone);
        std::erase(m_animating, &gone);
        if (m_focus == &gone)
        {
            m_focus = nullptr;
            m_focus_visible = false;
        }
        if (captured() == &gone)
            m_capture.reset();
        if (m_tooltip_for == &gone)
        {
            // Not hide_tooltip(): closing a popup hit-tests the tree, and the
            // tree may be half destroyed. The tooltip keeps its own text and
            // goes with the next press or move.
            if (m_tooltip_timer != 0)
                cancel(m_tooltip_timer);
            m_tooltip_timer = 0;
            m_tooltip_for = nullptr;
        }
        for (popup_entry &entry : m_popups)
        {
            if (entry.previous_focus == &gone)
                entry.previous_focus = nullptr;
            if (entry.options.owner == &gone)
                entry.options.owner = nullptr;
        }
    }

    // ---- input ---------------------------------------------------------------------------

    bool screen::handle(const mouse_button_event &e)
    {
        const math::vec2f position = logical(e.position);
        m_pointer = position;
        if (m_tooltip != nullptr || m_tooltip_timer != 0)
        {
            hide_tooltip();
            m_tooltip_for = nullptr;
        }

        pointer_event pe = pointer_from(position, e.mods);
        pe.button = e.button;
        pe.buttons = e.buttons;
        pe.clicks = e.clicks;

        bool handled = false;
        if (e.action == action::press)
        {
            if (m_swallowing)
                return true;

            if (!m_capture.captured())
            {
                widget *hit = hit_test(position);
                const int in_popup = popup_index_of(hit);

                if (!m_popups.empty())
                {
                    const std::size_t first = first_reachable_popup();
                    if (in_popup >= 0)
                    {
                        // Inside a popup: those opened after it close.
                        close_popups_from(static_cast<std::size_t>(in_popup) + 1);
                    }
                    else
                    {
                        const bool modal = m_popups[first].options.modal;
                        const std::size_t from = modal ? first + 1 : first;
                        bool closed = false;
                        for (std::size_t i = from; i < m_popups.size(); ++i)
                        {
                            if (m_popups[i].options.dismissable)
                            {
                                close_popups_from(i);
                                closed = true;
                                break;
                            }
                        }
                        if (modal || closed)
                        {
                            // The press only closed what was open.
                            m_swallowing = e.buttons != mouse_buttons::none;
                            update_cursor();
                            return true;
                        }
                    }
                }

                // A press focuses what it lands on, if that takes focus.
                for (widget *w = hit; w != nullptr; w = w->parent())
                {
                    if (w->focusable() && w->is_enabled())
                    {
                        set_focus(w, focus_reason::pointer);
                        break;
                    }
                }

                widget *handler = bubble(hit, [&](widget *w) { return w->on_pointer_down(pe); });
                m_capture.button(e, [handler] { return handler; });
                if (handler == nullptr)
                    m_capture.reset();
                handled = handler != nullptr;
            }
            else
            {
                widget *target = m_capture.button(e, [] { return static_cast<widget *>(nullptr); });
                if (target != nullptr && target->is_enabled())
                    handled = target->on_pointer_down(pe);
            }
        }
        else
        {
            if (m_swallowing)
            {
                if (e.buttons == mouse_buttons::none)
                    m_swallowing = false;
                return true;
            }

            const bool was_captured = m_capture.captured().has_value();
            widget *target = m_capture.button(e, [&] { return hit_test(position); });
            if (was_captured)
            {
                if (target != nullptr && target->is_enabled())
                    handled = target->on_pointer_up(pe);
            }
            else
            {
                handled = bubble(target, [&](widget *w) { return w->on_pointer_up(pe); }) != nullptr;
            }

            if (!m_capture.captured())
                update_hover(hit_test(position));
        }

        update_cursor();
        bury();
        return handled;
    }

    bool screen::handle(const mouse_move_event &e)
    {
        const math::vec2f position = logical(e.position);
        m_pointer = position;
        m_pointer_inside = true;
        if (m_swallowing)
            return true;

        pointer_event pe = pointer_from(position, e.mods);
        pe.delta = {static_cast<float>(e.delta.x()) / m_scale, static_cast<float>(e.delta.y()) / m_scale};
        pe.buttons = e.buttons;

        bool handled = false;
        if (widget *target = captured())
        {
            if (target->is_enabled())
            {
                target->on_pointer_move(pe);
                handled = true;
            }
        }
        else
        {
            widget *hit = hit_test(position);
            update_hover(hit);
            if (hit != nullptr && hit->is_enabled())
            {
                hit->on_pointer_move(pe);
                handled = true;
            }

            // Ancestors that follow the pointer over their children.
            const std::vector<widget *> chain = m_hover;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                if (*it != hit && (*it)->tracks_pointer() && (*it)->is_enabled())
                    (*it)->on_pointer_move(pe);
            }
        }

        update_cursor();
        bury();
        return handled;
    }

    bool screen::handle(const mouse_enter_event &e)
    {
        m_pointer = logical(e.position);
        m_pointer_inside = true;
        if (!m_capture.captured())
            update_hover(hit_test(m_pointer));
        update_cursor();
        return false;
    }

    bool screen::handle(const mouse_leave_event &e)
    {
        m_pointer = logical(e.position);
        m_pointer_inside = false;
        if (!m_capture.captured())
            update_hover(nullptr);
        hide_tooltip();
        update_cursor();
        return false;
    }

    bool screen::handle(const scroll_event &e)
    {
        const math::vec2f position = logical(e.position);
        m_pointer = position;
        hide_tooltip();

        scroll_input input;
        input.offset = {static_cast<float>(e.offset.x()), static_cast<float>(e.offset.y())};
        input.position = position;
        input.mods = e.mods;

        const bool handled = bubble(hit_test(position), [&](widget *w) { return w->on_scroll(input); }) != nullptr;
        bury();
        return handled;
    }

    bool screen::handle(const click_event &e)
    {
        if (m_swallowing)
            return true;

        pointer_event pe = pointer_from(logical(e.position), e.mods);
        pe.button = e.button;
        pe.clicks = e.clicks;

        bool handled = false;
        if (widget *target = captured())
            handled = target->is_enabled() && target->on_click(pe);
        else
            handled = bubble(hit_test(pe.position), [&](widget *w) { return w->on_click(pe); }) != nullptr;
        bury();
        return handled;
    }

    bool screen::handle(const drag_begin_event &e)
    {
        if (m_swallowing)
            return true;

        pointer_event pe = pointer_from(logical(e.position), e.mods);
        pe.button = e.button;
        pe.origin = logical(e.origin);

        bool handled = false;
        if (widget *target = captured())
            handled = target->is_enabled() && target->on_drag_begin(pe);
        else
            handled = bubble(hit_test(pe.origin), [&](widget *w) { return w->on_drag_begin(pe); }) != nullptr;
        bury();
        return handled;
    }

    bool screen::handle(const drag_move_event &e)
    {
        if (m_swallowing)
            return true;

        pointer_event pe = pointer_from(logical(e.position), e.mods);
        pe.button = e.button;
        pe.origin = logical(e.origin);
        pe.delta = {static_cast<float>(e.delta.x()) / m_scale, static_cast<float>(e.delta.y()) / m_scale};

        bool handled = false;
        if (widget *target = captured())
            handled = target->is_enabled() && target->on_drag_move(pe);
        bury();
        return handled;
    }

    bool screen::handle(const drag_end_event &e)
    {
        if (m_swallowing)
            return true;

        pointer_event pe = pointer_from(logical(e.position), e.mods);
        pe.button = e.button;
        pe.origin = logical(e.origin);
        pe.cancelled = e.cancelled;

        bool handled = false;
        if (widget *target = captured())
            handled = target->is_enabled() && target->on_drag_end(pe);
        bury();
        return handled;
    }

    bool screen::handle(const key_event &e)
    {
        if (e.action != action::release)
            hide_tooltip();

        widget *target = m_focus;
        if (target == nullptr && !m_popups.empty() && m_popups[first_reachable_popup()].options.modal)
            target = m_popups[first_reachable_popup()].content.get();

        bool handled = bubble(target, [&](widget *w) { return w->on_key(e); }) != nullptr;
        if (!handled && e.action != action::release)
            handled = handle_unhandled_key(e);
        bury();
        return handled;
    }

    bool screen::handle_unhandled_key(const key_event &e)
    {
        const modifiers held = e.mods & held_modifiers;

        if (e.key == key::escape && held == modifiers::none)
        {
            for (std::size_t i = m_popups.size(); i-- > 0;)
            {
                if (m_popups[i].options.dismissable)
                {
                    close_popups_from(i);
                    return true;
                }
            }
        }

        if (e.key == key::tab && (held == modifiers::none || held == modifiers::shift))
            return focus_next(held == modifiers::shift);

        // No shortcuts behind a modal dialog.
        if (!m_popups.empty() && m_popups[first_reachable_popup()].options.modal)
            return false;

        if (e.action == action::press)
        {
            if (const auto command = m_shortcuts.match(e))
                return run_command(*command);
        }
        return false;
    }

    bool screen::handle(const text_event &e)
    {
        const bool handled = bubble(m_focus, [&](widget *w) { return w->on_text(e); }) != nullptr;
        bury();
        return handled;
    }

    bool screen::handle(const focus_event &e)
    {
        if (!e.focused)
        {
            // Whatever was held has been released by now (synthetic
            // events); what a press opened stays, except menus and lists.
            m_capture.reset();
            m_swallowing = false;
            hide_tooltip();
            for (std::size_t i = first_reachable_popup(); i < m_popups.size(); ++i)
            {
                if (m_popups[i].options.dismissable && !m_popups[i].options.modal)
                {
                    close_popups_from(i);
                    break;
                }
            }
        }
        bury();
        return false;
    }

    std::vector<events::scoped_token> screen::connect(events::bus &bus)
    {
        std::vector<events::scoped_token> tokens;
        const auto listen = [&]<typename Event>(std::type_identity<Event>) {
            tokens.emplace_back(bus.add_listener<Event>([this](const Event &e) { handle(e); }));
        };
        listen(std::type_identity<mouse_button_event>{});
        listen(std::type_identity<mouse_move_event>{});
        listen(std::type_identity<mouse_enter_event>{});
        listen(std::type_identity<mouse_leave_event>{});
        listen(std::type_identity<scroll_event>{});
        listen(std::type_identity<click_event>{});
        listen(std::type_identity<drag_begin_event>{});
        listen(std::type_identity<drag_move_event>{});
        listen(std::type_identity<drag_end_event>{});
        listen(std::type_identity<key_event>{});
        listen(std::type_identity<text_event>{});
        listen(std::type_identity<focus_event>{});
        return tokens;
    }

} // namespace volt::ui
