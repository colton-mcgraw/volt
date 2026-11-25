#include <ui/widgets/scroll_area.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        // How far one notch of the wheel scrolls, logical units.
        constexpr float wheel_step = 48.0f;
        constexpr float minimum_thumb = 20.0f;
    } // namespace

    // ---- scrollbar --------------------------------------------------------------

    void scrollbar::set_extent(float content, float view)
    {
        m_content = std::max(0.0f, content);
        m_view = std::max(0.0f, view);
        m_offset = std::clamp(m_offset, 0.0f, max_offset());
    }

    bool scrollbar::set_offset(float offset) noexcept
    {
        offset = std::clamp(std::round(offset), 0.0f, max_offset());
        if (offset == m_offset)
            return false;
        m_offset = offset;
        return true;
    }

    rect scrollbar::thumb(const rect &bar) const noexcept
    {
        const bool vertical = m_orientation == orientation::vertical;
        const float length = vertical ? bar.height : bar.width;
        if (m_content <= 0.0f || !needed())
            return bar;

        const float size = std::clamp(length * m_view / m_content, std::min(minimum_thumb, length), length);
        const float travel = length - size;
        const float position = max_offset() > 0.0f ? travel * m_offset / max_offset() : 0.0f;
        if (vertical)
            return {bar.x, bar.y + position, bar.width, size};
        return {bar.x + position, bar.y, size, bar.height};
    }

    bool scrollbar::press(const math::vec2f &point, const rect &bar) noexcept
    {
        const rect t = thumb(bar);
        if (t.contains(point))
        {
            m_grab = along(point) - along(t.position());
            return false;
        }

        // The track: a page towards the press, and then the thumb follows.
        const float page = std::max(1.0f, m_view - 24.0f);
        const bool before = along(point) < along(t.position());
        const bool moved = scroll_by(before ? -page : page);
        m_grab = along(thumb(bar).size()) * 0.5f;
        return moved;
    }

    bool scrollbar::drag(const math::vec2f &point, const rect &bar) noexcept
    {
        if (!m_grab)
            return false;
        const rect t = thumb(bar);
        const float length = m_orientation == orientation::vertical ? bar.height : bar.width;
        const float travel = length - along(t.size());
        if (travel <= 0.0f)
            return false;
        const float start = m_orientation == orientation::vertical ? bar.y : bar.x;
        const float position = std::clamp(along(point) - *m_grab - start, 0.0f, travel);
        return set_offset(position / travel * max_offset());
    }

    void scrollbar::paint(painter &p, const rect &bar, bool hot) const
    {
        if (!needed())
            return;
        const ui::theme &t = p.theme();

        // Thin until the pointer comes near, as modern bars are.
        const bool wide = hot || dragging();
        const float thickness = wide ? 8.0f : 5.0f;
        const rect r = thumb(bar);
        const rect shown = m_orientation == orientation::vertical
                               ? rect{bar.right() - thickness - 2.0f, r.y + 2.0f, thickness, r.height - 4.0f}
                               : rect{r.x + 2.0f, bar.bottom() - thickness - 2.0f, r.width - 4.0f, thickness};
        if (wide)
            p.fill(m_orientation == orientation::vertical
                       ? rect{bar.right() - thickness - 3.0f, bar.y, thickness + 2.0f, bar.height}
                       : rect{bar.x, bar.bottom() - thickness - 3.0f, bar.width, thickness + 2.0f},
                   faded(t.scrollbar_thumb, 0.25f), thickness * 0.5f);
        p.fill(shown, wide ? t.scrollbar_thumb_hover : t.scrollbar_thumb, thickness * 0.5f);
    }

    // ---- scroll_area --------------------------------------------------------------

    void scroll_area::set_offset(const math::vec2f &offset)
    {
        const bool moved_x = m_horizontal.set_offset(offset.x());
        const bool moved_y = m_vertical.set_offset(offset.y());
        if (moved_x || moved_y)
        {
            place_content();
            invalidate();
        }
    }

    void scroll_area::scroll_by(const math::vec2f &delta)
    {
        set_offset(offset() + delta);
    }

    void scroll_area::scroll_into_view(const rect &area)
    {
        math::vec2f target = offset();
        if (area.y < m_viewport.y)
            target[1] -= m_viewport.y - area.y;
        else if (area.bottom() > m_viewport.bottom())
            target[1] += std::min(area.bottom() - m_viewport.bottom(), area.y - m_viewport.y);
        if (area.x < m_viewport.x)
            target[0] -= m_viewport.x - area.x;
        else if (area.right() > m_viewport.right())
            target[0] += std::min(area.right() - m_viewport.right(), area.x - m_viewport.x);
        set_offset(target);
    }

    rect scroll_area::vertical_rect() const noexcept
    {
        const float width = theme().scrollbar_width + 2.0f;
        return {bounds().right() - width, bounds().y, width, m_viewport.height};
    }

    rect scroll_area::horizontal_rect() const noexcept
    {
        const float height = theme().scrollbar_width + 2.0f;
        return {bounds().x, bounds().bottom() - height, m_viewport.width, height};
    }

    math::vec2f scroll_area::on_measure(const constraints &limits)
    {
        widget *inner = content();
        if (inner == nullptr)
            return {0.0f, 0.0f};

        constraints offered = constraints::loose(limits.max);
        if (m_scroll_vertical)
            offered.max[1] = unbounded;
        if (m_scroll_horizontal)
            offered.max[0] = unbounded;
        const math::vec2f size = inner->measure(offered);
        return {std::min(size.x(), limits.max.x()), std::min(size.y(), limits.max.y())};
    }

    void scroll_area::on_arrange(const rect &bounds)
    {
        widget *inner = content();
        m_viewport = bounds;
        if (inner == nullptr)
            return;

        const float bar = theme().scrollbar_width + 2.0f;

        // Measure as if no bar were needed; if one is, it takes room from
        // the other axis, and the content is measured again in what is left.
        const auto measure_in = [&](float width, float height) {
            constraints offered = constraints::loose({width, height});
            if (m_scroll_vertical)
                offered.max[1] = unbounded;
            if (m_scroll_horizontal)
                offered.max[0] = unbounded;
            return inner->measure(offered);
        };

        float width = bounds.width;
        float height = bounds.height;
        math::vec2f size = measure_in(width, height);
        const bool need_vertical = m_scroll_vertical && size.y() > height + 0.5f;
        if (need_vertical)
        {
            width = std::max(0.0f, width - bar);
            size = measure_in(width, height);
        }
        const bool need_horizontal = m_scroll_horizontal && size.x() > width + 0.5f;
        if (need_horizontal)
            height = std::max(0.0f, height - bar);

        m_viewport = {bounds.x, bounds.y, width, height};
        m_content_size = {m_scroll_horizontal ? std::max(size.x(), width) : width,
                          m_scroll_vertical ? std::max(size.y(), height) : height};
        m_vertical.set_extent(m_scroll_vertical ? m_content_size.y() : 0.0f, height);
        m_horizontal.set_extent(m_scroll_horizontal ? m_content_size.x() : 0.0f, width);
        place_content();
    }

    void scroll_area::place_content()
    {
        if (widget *inner = content())
        {
            inner->arrange({m_viewport.x - m_horizontal.offset(), m_viewport.y - m_vertical.offset(),
                            m_content_size.x(), m_content_size.y()});
        }
    }

    void scroll_area::on_paint(painter &p)
    {
        {
            clip_scope clip(p, m_viewport);
            paint_children(p);
        }
        m_vertical.paint(p, vertical_rect(), m_hot_bar == 1);
        m_horizontal.paint(p, horizontal_rect(), m_hot_bar == 2);
    }

    widget *scroll_area::hit_test(const math::vec2f &point)
    {
        if (!visible() || !bounds().contains(point))
            return nullptr;
        if ((m_vertical.needed() && vertical_rect().contains(point)) ||
            (m_horizontal.needed() && horizontal_rect().contains(point)) || !m_viewport.contains(point))
            return this;
        if (widget *inner = content())
        {
            if (widget *hit = inner->hit_test(point))
                return hit;
        }
        return this;
    }

    bool scroll_area::on_scroll(const scroll_input &e)
    {
        math::vec2f delta{-e.offset.x() * wheel_step, -e.offset.y() * wheel_step};
        if (has(e.mods, modifiers::shift) || !m_scroll_vertical)
            delta = {delta.x() + delta.y(), 0.0f};

        const math::vec2f before = offset();
        scroll_by(delta);
        // At the end already: let an enclosing scroller have it.
        return offset() != before;
    }

    bool scroll_area::on_pointer_down(const pointer_event &e)
    {
        if (e.button != mouse_button::left)
            return false;
        bool moved = false;
        if (m_vertical.needed() && vertical_rect().contains(e.position))
            moved = m_vertical.press(e.position, vertical_rect());
        else if (m_horizontal.needed() && horizontal_rect().contains(e.position))
            moved = m_horizontal.press(e.position, horizontal_rect());
        else
            return false;
        if (moved)
            place_content();
        invalidate();
        return true;
    }

    void scroll_area::on_pointer_move(const pointer_event &e)
    {
        bool moved = false;
        if (m_vertical.dragging())
            moved = m_vertical.drag(e.position, vertical_rect());
        else if (m_horizontal.dragging())
            moved = m_horizontal.drag(e.position, horizontal_rect());

        const int hot = m_vertical.needed() && vertical_rect().contains(e.position)       ? 1
                        : m_horizontal.needed() && horizontal_rect().contains(e.position) ? 2
                                                                                          : 0;
        if (moved)
            place_content();
        if (moved || hot != m_hot_bar)
        {
            m_hot_bar = hot;
            invalidate();
        }
    }

    bool scroll_area::on_pointer_up(const pointer_event &e)
    {
        if (!m_vertical.dragging() && !m_horizontal.dragging())
            return false;
        m_vertical.release();
        m_horizontal.release();
        if (!bounds().contains(e.position))
            m_hot_bar = 0;
        invalidate();
        return true;
    }

    void scroll_area::on_pointer_leave()
    {
        if (m_hot_bar != 0)
        {
            m_hot_bar = 0;
            invalidate();
        }
    }

} // namespace volt::ui
