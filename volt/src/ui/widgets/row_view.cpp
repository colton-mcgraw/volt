#include <ui/widgets/row_view.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{
    namespace
    {
        constexpr float wheel_rows = 3.0f;
    } // namespace

    row_view::row_view()
    {
        set_focusable(true);
    }

    float row_view::row_height() const
    {
        return theme().row_height;
    }

    rect row_view::list_rect() const
    {
        rect area = bounds().inset(insets{1.0f, 1.0f + m_top_margin, 1.0f, 1.0f});
        if (m_bar.needed())
            area.width = std::max(0.0f, area.width - theme().scrollbar_width);
        return area;
    }

    rect row_view::bar_rect() const
    {
        const rect inner = bounds().inset(insets{1.0f, 1.0f + m_top_margin, 1.0f, 1.0f});
        const float width = theme().scrollbar_width + 2.0f;
        return {inner.right() - width, inner.y, width, inner.height};
    }

    void row_view::set_top_margin(float margin)
    {
        margin = std::max(0.0f, margin);
        if (margin == m_top_margin)
            return;
        m_top_margin = margin;
        invalidate_layout();
    }

    std::pair<std::size_t, std::size_t> row_view::visible_range() const
    {
        const float height = row_height();
        const std::size_t count = row_count();
        if (height <= 0.0f || count == 0)
            return {0, 0};
        const auto first = static_cast<std::size_t>(std::floor(m_bar.offset() / height));
        const auto last = static_cast<std::size_t>(std::ceil((m_bar.offset() + list_rect().height) / height));
        return {std::min(first, count), std::min(last, count)};
    }

    rect row_view::row_rect(std::size_t index) const
    {
        const rect area = list_rect();
        const float height = row_height();
        return {area.x, area.y + static_cast<float>(index) * height - m_bar.offset(), area.width, height};
    }

    int row_view::row_at(const math::vec2f &point) const
    {
        const rect area = list_rect();
        if (!area.contains(point))
            return -1;
        const auto index = static_cast<std::size_t>((point.y() - area.y + m_bar.offset()) / row_height());
        return index < row_count() ? static_cast<int>(index) : -1;
    }

    void row_view::rows_changed()
    {
        if (m_selected >= static_cast<int>(row_count()))
            m_selected = static_cast<int>(row_count()) - 1;
        m_hot = -1;
        if (owner() != nullptr)
            m_bar.set_extent(static_cast<float>(row_count()) * row_height(),
                             std::max(0.0f, bounds().height - 2.0f - m_top_margin));
        invalidate_layout();
    }

    void row_view::scroll_to(float offset)
    {
        if (m_bar.set_offset(offset))
            invalidate();
    }

    void row_view::ensure_visible(int index)
    {
        if (index < 0)
        {
            m_pending_visible = -1;
            return;
        }
        // Not shown yet, or not laid out: once it is.
        if (owner() == nullptr || bounds().empty())
        {
            m_pending_visible = index;
            return;
        }
        m_pending_visible = -1;
        const float height = row_height();
        const float top = static_cast<float>(index) * height;
        const float view = list_rect().height;
        if (top < m_bar.offset())
            scroll_to(top);
        else if (top + height > m_bar.offset() + view)
            scroll_to(top + height - view);
    }

    void row_view::set_selected(int index)
    {
        index = std::clamp(index, -1, static_cast<int>(row_count()) - 1);
        if (index != m_selected)
        {
            m_selected = index;
            invalidate();
        }
        ensure_visible(index);
    }

    void row_view::choose(int index)
    {
        index = std::clamp(index, -1, static_cast<int>(row_count()) - 1);
        ensure_visible(index);
        if (index == m_selected)
            return;
        m_selected = index;
        invalidate();
        selection_changed.emit(index);
    }

    math::vec2f row_view::on_measure(const constraints &)
    {
        const float rows = static_cast<float>(std::min<std::size_t>(row_count(), 8));
        return {200.0f, std::max(row_height() * 3.0f, rows * row_height()) + 2.0f + m_top_margin};
    }

    void row_view::on_arrange(const rect &bounds)
    {
        m_bar.set_extent(static_cast<float>(row_count()) * row_height(),
                         std::max(0.0f, bounds.height - 2.0f - m_top_margin));
        if (m_pending_visible >= 0)
            ensure_visible(std::min(m_pending_visible, static_cast<int>(row_count()) - 1));
    }

    void row_view::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        p.frame(b, t.field, has_focus() ? mix(t.border, t.accent, 0.6f) : t.border, t.radius);

        const rect area = list_rect();
        m_painted = 0;
        {
            clip_scope clip(p, area);
            const auto [first, last] = visible_range();
            for (std::size_t i = first; i < last; ++i)
            {
                const rect r = row_rect(i);
                if (static_cast<int>(i) == m_selected)
                    p.fill(r.inset(insets::symmetric(2.0f, 0.0f)), has_focus() ? t.selection : t.selection_inactive,
                           t.small_radius);
                else if (static_cast<int>(i) == m_hot)
                    p.fill(r.inset(insets::symmetric(2.0f, 0.0f)), t.row_hover, t.small_radius);
                paint_row(p, i, r);
                ++m_painted;
            }
        }
        m_bar.paint(p, bar_rect(), m_bar_hot);
    }

    bool row_view::on_scroll(const scroll_input &e)
    {
        const float before = m_bar.offset();
        scroll_to(before - e.offset.y() * wheel_rows * row_height());
        if (m_bar.offset() == before)
            return false;
        // The row under the pointer is another one now.
        m_hot = row_at(e.position);
        return true;
    }

    bool row_view::on_pointer_down(const pointer_event &e)
    {
        if (m_bar.needed() && bar_rect().contains(e.position))
        {
            if (e.button == mouse_button::left)
            {
                m_bar.press(e.position, bar_rect());
                invalidate();
            }
            return true;
        }

        const int index = row_at(e.position);
        if (index >= 0 && row_pressed(static_cast<std::size_t>(index), e))
            return true;
        if (e.button == mouse_button::left || e.button == mouse_button::right)
            choose(index);
        return true;
    }

    void row_view::on_pointer_move(const pointer_event &e)
    {
        if (m_bar.dragging())
        {
            if (m_bar.drag(e.position, bar_rect()))
                invalidate();
            return;
        }
        const bool bar_hot = m_bar.needed() && bar_rect().contains(e.position);
        const int hot = bar_hot ? -1 : row_at(e.position);
        if (hot != m_hot || bar_hot != m_bar_hot)
        {
            m_hot = hot;
            m_bar_hot = bar_hot;
            invalidate();
        }
    }

    bool row_view::on_pointer_up(const pointer_event &)
    {
        if (m_bar.dragging())
        {
            m_bar.release();
            invalidate();
        }
        return true;
    }

    void row_view::on_pointer_leave()
    {
        if (m_hot != -1 || m_bar_hot)
            invalidate();
        m_hot = -1;
        m_bar_hot = false;
    }

    bool row_view::on_click(const pointer_event &e)
    {
        if (e.button == mouse_button::left && e.clicks == 2)
        {
            const int index = row_at(e.position);
            if (index >= 0 && index == m_selected)
            {
                activated.emit(index);
                return true;
            }
        }
        return false;
    }

    bool row_view::on_key(const key_event &e)
    {
        if (e.action == action::release)
            return false;
        if (row_key(e))
            return true;
        if ((e.mods & held_modifiers) != modifiers::none)
            return false;

        const int count = static_cast<int>(row_count());
        if (count == 0)
            return false;
        const int page = std::max(1, static_cast<int>(list_rect().height / row_height()) - 1);

        switch (e.key)
        {
        case key::down:
            choose(std::min(count - 1, m_selected + 1));
            return true;
        case key::up:
            choose(std::max(0, m_selected - 1));
            return true;
        case key::page_down:
            choose(std::min(count - 1, std::max(0, m_selected) + page));
            return true;
        case key::page_up:
            choose(std::max(0, m_selected - page));
            return true;
        case key::home:
            choose(0);
            return true;
        case key::end:
            choose(count - 1);
            return true;
        case key::enter:
        case key::kp_enter:
            if (m_selected >= 0)
                activated.emit(m_selected);
            return m_selected >= 0;
        default:
            return false;
        }
    }

} // namespace volt::ui
