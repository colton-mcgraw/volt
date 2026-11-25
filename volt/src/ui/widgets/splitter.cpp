#include <ui/widgets/splitter.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        // The divider can be grabbed this far to either side of its line.
        constexpr float slop = 3.0f;
    } // namespace

    splitter::splitter(ui::orientation orientation) : m_orientation(orientation)
    {
        set_tracks_pointer(true);
    }

    void splitter::replace(int index, std::unique_ptr<widget> pane)
    {
        widget *&slot = index == 0 ? m_first : m_second;
        if (slot != nullptr)
        {
            remove(*slot);
            slot = nullptr;
        }
        if (!pane)
            return;
        slot = pane.get();
        insert_child(index == 0 ? 0 : child_count(), std::move(pane));
    }

    void splitter::set_ratio(float ratio)
    {
        m_keep = keep::ratio;
        m_ratio = std::clamp(ratio, 0.0f, 1.0f);
        invalidate_layout();
    }

    void splitter::set_fixed(keep which, float size)
    {
        m_keep = which;
        m_fixed = std::max(0.0f, size);
        invalidate_layout();
    }

    void splitter::set_minimums(float first, float second)
    {
        m_min_first = first;
        m_min_second = second;
        invalidate_layout();
    }

    void splitter::set_collapsed(pane which)
    {
        if (which == m_collapsed)
            return;
        m_collapsed = which;
        // A drag in progress ends where it is.
        m_dragging = false;
        m_hot = false;
        if (m_first != nullptr)
            m_first->set_visible(which != pane::first);
        if (m_second != nullptr)
            m_second->set_visible(which != pane::second);
        invalidate_layout();
        invalidate();
    }

    float splitter::length() const noexcept
    {
        return m_orientation == orientation::horizontal ? bounds().width : bounds().height;
    }

    float splitter::clamp_position(float position) const noexcept
    {
        const float gap = theme().splitter_width;
        const float available = std::max(0.0f, length() - gap);
        const float low = std::min(m_min_first, available);
        const float high = std::max(low, available - m_min_second);
        return std::round(std::clamp(position, low, high));
    }

    rect splitter::divider() const noexcept
    {
        if (m_collapsed != pane::none)
            return {bounds().x, bounds().y, 0.0f, 0.0f};
        const float gap = theme().splitter_width;
        const rect &b = bounds();
        return m_orientation == orientation::horizontal ? rect{b.x + m_position, b.y, gap, b.height}
                                                        : rect{b.x, b.y + m_position, b.width, gap};
    }

    math::vec2f splitter::on_measure(const constraints &limits)
    {
        // One pane collapsed: the other is all there is.
        if (m_collapsed != pane::none)
        {
            widget *shown = m_collapsed == pane::first ? m_second : m_first;
            return shown != nullptr ? shown->measure(constraints::loose(limits.max)) : math::vec2f{};
        }

        const float gap = theme().splitter_width;

        // Each pane is offered the length it will be given, as far as the
        // limits tell: what wraps (text, tiles) is then measured at the width
        // it is laid out at, and what follows it in the pane is placed after
        // its real height. Placed as on_arrange() places the divider.
        constraints first = constraints::loose(limits.max);
        constraints second = first;
        const std::size_t axis = m_orientation == orientation::horizontal ? 0 : 1;
        if (std::isfinite(limits.max[axis]))
        {
            const float available = std::max(0.0f, limits.max[axis] - gap);
            float position = m_keep == keep::ratio   ? available * m_ratio
                             : m_keep == keep::first ? m_fixed
                                                     : available - m_fixed;
            const float low = std::min(m_min_first, available);
            const float high = std::max(low, available - m_min_second);
            position = std::round(std::clamp(position, low, high));
            first.max[axis] = position;
            second.max[axis] = std::max(0.0f, available - position);
        }

        math::vec2f a{}, b{};
        if (m_first != nullptr)
            a = m_first->measure(first);
        if (m_second != nullptr)
            b = m_second->measure(second);
        if (m_orientation == orientation::horizontal)
            return {a.x() + gap + b.x(), std::max(a.y(), b.y())};
        return {std::max(a.x(), b.x()), a.y() + gap + b.y()};
    }

    void splitter::on_arrange(const rect &bounds)
    {
        // One pane collapsed: the other has the whole of it, the hidden one
        // nothing, and the divider's place is left as it was.
        if (m_collapsed != pane::none)
        {
            widget *shown = m_collapsed == pane::first ? m_second : m_first;
            widget *hidden = m_collapsed == pane::first ? m_first : m_second;
            if (shown != nullptr)
                shown->arrange(bounds);
            if (hidden != nullptr)
                hidden->arrange({bounds.x, bounds.y, 0.0f, 0.0f});
            return;
        }

        const float gap = theme().splitter_width;
        const float total = m_orientation == orientation::horizontal ? bounds.width : bounds.height;
        const float available = std::max(0.0f, total - gap);

        float position = 0.0f;
        switch (m_keep)
        {
        case keep::ratio:
            position = available * m_ratio;
            break;
        case keep::first:
            position = m_fixed;
            break;
        case keep::second:
            position = available - m_fixed;
            break;
        }
        m_position = clamp_position(position);

        const float rest = std::max(0.0f, available - m_position);
        if (m_orientation == orientation::horizontal)
        {
            if (m_first != nullptr)
                m_first->arrange({bounds.x, bounds.y, m_position, bounds.height});
            if (m_second != nullptr)
                m_second->arrange({bounds.x + m_position + gap, bounds.y, rest, bounds.height});
        }
        else
        {
            if (m_first != nullptr)
                m_first->arrange({bounds.x, bounds.y, bounds.width, m_position});
            if (m_second != nullptr)
                m_second->arrange({bounds.x, bounds.y + m_position + gap, bounds.width, rest});
        }
    }

    void splitter::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        paint_children(p);
        if (m_collapsed != pane::none)
            return;

        const rect d = divider();
        if (m_dragging || m_hot)
        {
            p.fill(d, faded(t.accent, m_dragging ? 0.8f : 0.5f));
        }
        else
        {
            // A line down the middle; the rest of the gap is background.
            if (m_orientation == orientation::horizontal)
                p.fill({std::floor(d.center().x()), d.y, 1.0f, d.height}, t.separator);
            else
                p.fill({d.x, std::floor(d.center().y()), d.width, 1.0f}, t.separator);
        }
    }

    bool splitter::over_divider(const math::vec2f &point) const noexcept
    {
        if (m_collapsed != pane::none)
            return false;
        return divider().outset(slop).contains(point) && bounds().contains(point);
    }

    widget *splitter::hit_test(const math::vec2f &point)
    {
        if (!visible() || !bounds().contains(point))
            return nullptr;
        if (over_divider(point))
            return this;
        return widget::hit_test(point);
    }

    cursor_shape splitter::cursor(const math::vec2f &point) const
    {
        if (m_dragging || over_divider(point))
            return m_orientation == orientation::horizontal ? cursor_shape::resize_horizontal
                                                            : cursor_shape::resize_vertical;
        return cursor_shape::arrow;
    }

    void splitter::drag_to(float position)
    {
        position = clamp_position(position);
        if (position == m_position)
            return;

        const float gap = theme().splitter_width;
        const float available = std::max(1.0f, length() - gap);
        switch (m_keep)
        {
        case keep::ratio:
            m_ratio = position / available;
            break;
        case keep::first:
            m_fixed = position;
            break;
        case keep::second:
            m_fixed = available - position;
            break;
        }
        on_arrange(bounds());
        invalidate();
        moved.emit(m_position);
    }

    bool splitter::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left || !over_divider(e.position))
            return false;
        m_dragging = true;
        const float along = m_orientation == orientation::horizontal ? e.position.x() - bounds().x
                                                                     : e.position.y() - bounds().y;
        m_grab = along - m_position;
        invalidate();
        return true;
    }

    void splitter::on_pointer_move(const pointer_event &e)
    {
        if (m_dragging)
        {
            const float along = m_orientation == orientation::horizontal ? e.position.x() - bounds().x
                                                                         : e.position.y() - bounds().y;
            drag_to(along - m_grab);
            return;
        }
        const bool hot = over_divider(e.position);
        if (hot != m_hot)
        {
            m_hot = hot;
            invalidate();
        }
    }

    bool splitter::on_pointer_up(const pointer_event &e)
    {
        if (!m_dragging || e.button != mouse_button::left)
            return false;
        m_dragging = false;
        m_hot = over_divider(e.position);
        invalidate();
        return true;
    }

} // namespace volt::ui
