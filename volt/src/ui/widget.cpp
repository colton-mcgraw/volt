#include <ui/widget.hpp>

#include <ui/painter.hpp>
#include <ui/screen.hpp>

#include <algorithm>
#include <cassert>

namespace volt::ui
{

    widget::~widget()
    {
        // Children go first, while they can still tell the screen.
        m_children.clear();
        if (m_screen != nullptr)
            m_screen->forget(*this);
    }

    // ---- tree ---------------------------------------------------------------

    void widget::insert_child(std::size_t index, std::unique_ptr<widget> child)
    {
        assert(child != nullptr && child->m_parent == nullptr && "a widget has one parent");

        widget &added = *child;
        added.m_parent = this;
        m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(std::min(index, m_children.size())),
                          std::move(child));
        if (m_screen != nullptr)
            added.attach(m_screen);
        invalidate_layout();
    }

    std::unique_ptr<widget> widget::remove(widget &child)
    {
        const auto found = std::ranges::find_if(m_children, [&](const auto &c) { return c.get() == &child; });
        if (found == m_children.end())
            return nullptr;

        std::unique_ptr<widget> removed = std::move(*found);
        m_children.erase(found);
        removed->detach();
        removed->m_parent = nullptr;
        invalidate_layout();
        return removed;
    }

    void widget::clear_children()
    {
        for (auto &child : m_children)
            child->detach();
        m_children.clear();
        invalidate_layout();
    }

    bool widget::is_ancestor_of(const widget &other) const noexcept
    {
        for (const widget *w = other.m_parent; w != nullptr; w = w->m_parent)
        {
            if (w == this)
                return true;
        }
        return false;
    }

    void widget::attach(ui::screen *screen)
    {
        m_screen = screen;
        for (auto &child : m_children)
            child->attach(screen);
        on_attached();
    }

    void widget::detach()
    {
        if (m_screen == nullptr)
            return;
        for (auto &child : m_children)
            child->detach();
        on_detached();
        m_screen->forget(*this);
        m_screen = nullptr;
        m_hovered = false;
    }

    widget *widget::find(std::string_view name) noexcept
    {
        if (m_name == name)
            return this;
        for (auto &child : m_children)
        {
            if (widget *found = child->find(name))
                return found;
        }
        return nullptr;
    }

    // ---- state ----------------------------------------------------------------

    void widget::set_visible(bool visible)
    {
        if (m_visible == visible)
            return;
        m_visible = visible;
        if (!visible && m_screen != nullptr)
        {
            // Whatever pointed into the hidden part lets go.
            if (widget *focus = m_screen->focused(); focus != nullptr && (focus == this || is_ancestor_of(*focus)))
                m_screen->set_focus(nullptr);
        }
        invalidate_layout();
    }

    bool widget::is_enabled() const noexcept
    {
        for (const widget *w = this; w != nullptr; w = w->m_parent)
        {
            if (!w->m_enabled)
                return false;
        }
        return true;
    }

    void widget::set_enabled(bool enabled)
    {
        if (m_enabled == enabled)
            return;
        m_enabled = enabled;
        if (!enabled && m_screen != nullptr)
        {
            if (widget *focus = m_screen->focused(); focus != nullptr && (focus == this || is_ancestor_of(*focus)))
                m_screen->set_focus(nullptr);
        }
        invalidate();
    }

    bool widget::has_focus() const noexcept
    {
        return m_screen != nullptr && m_screen->focused() == this;
    }

    // ---- layout -----------------------------------------------------------------

    math::vec2f widget::measure(const constraints &limits)
    {
        if (!m_visible)
        {
            m_measured = {0.0f, 0.0f};
            return m_measured;
        }

        // Fixed sizes narrow the constraints the content is measured in.
        constraints inner = limits;
        if (m_layout.width >= 0.0f)
            inner.min[0] = inner.max[0] = std::clamp(m_layout.width, limits.min.x(), limits.max.x());
        if (m_layout.height >= 0.0f)
            inner.min[1] = inner.max[1] = std::clamp(m_layout.height, limits.min.y(), limits.max.y());
        inner.max[0] = std::min(inner.max.x(), std::max(m_layout.max_size.x(), inner.min.x()));
        inner.max[1] = std::min(inner.max.y(), std::max(m_layout.max_size.y(), inner.min.y()));

        math::vec2f size = on_measure(inner);
        if (m_layout.width >= 0.0f)
            size[0] = m_layout.width;
        if (m_layout.height >= 0.0f)
            size[1] = m_layout.height;
        size[0] = std::clamp(size.x(), m_layout.min_size.x(), std::max(m_layout.min_size.x(), m_layout.max_size.x()));
        size[1] = std::clamp(size.y(), m_layout.min_size.y(), std::max(m_layout.min_size.y(), m_layout.max_size.y()));

        m_measured = limits.constrain(size);
        return m_measured;
    }

    void widget::arrange(const rect &bounds)
    {
        if (!m_visible)
        {
            m_bounds = {bounds.x, bounds.y, 0.0f, 0.0f};
            return;
        }
        m_bounds = bounds;
        on_arrange(bounds);
    }

    math::vec2f widget::on_measure(const constraints &limits)
    {
        math::vec2f size{0.0f, 0.0f};
        for (auto &child : m_children)
        {
            const math::vec2f measured = child->measure(constraints::loose(limits.max));
            size[0] = std::max(size.x(), measured.x());
            size[1] = std::max(size.y(), measured.y());
        }
        return size;
    }

    void widget::on_arrange(const rect &bounds)
    {
        for (auto &child : m_children)
            child->arrange(bounds);
    }

    void widget::invalidate()
    {
        if (m_screen != nullptr)
            m_screen->request_redraw();
    }

    void widget::invalidate_layout()
    {
        if (m_screen != nullptr)
            m_screen->invalidate_layout();
    }

    // ---- painting and hit testing ---------------------------------------------------

    void widget::paint(painter &p)
    {
        if (!m_visible || m_bounds.empty())
            return;
        if (!m_bounds.intersects(p.visible()))
            return;
        on_paint(p);
    }

    void widget::on_paint(painter &p)
    {
        paint_children(p);
    }

    void widget::paint_children(painter &p)
    {
        for (auto &child : m_children)
            child->paint(p);
    }

    widget *widget::hit_test(const math::vec2f &point)
    {
        if (!m_visible || !m_bounds.contains(point))
            return nullptr;

        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
        {
            if (widget *hit = (*it)->hit_test(point))
                return hit;
        }
        return m_hit_testable ? this : nullptr;
    }

    cursor_shape widget::cursor(const math::vec2f &) const
    {
        return cursor_shape::arrow;
    }

    // ---- services -----------------------------------------------------------------------

    const ui::theme &widget::theme() const
    {
        assert(m_screen != nullptr && "a widget's theme is only there while it is shown");
        return m_screen->theme();
    }

    text_engine &widget::fonts() const
    {
        assert(m_screen != nullptr && "a widget's text engine is only there while it is shown");
        return m_screen->text();
    }

    void widget::animate()
    {
        if (m_screen != nullptr)
            m_screen->start_animation(*this);
    }

    void widget::focus(focus_reason reason)
    {
        if (m_screen != nullptr)
            m_screen->set_focus(this, reason);
    }

    void widget::capture_pointer()
    {
        if (m_screen != nullptr)
            m_screen->capture(*this);
    }

    void widget::release_pointer()
    {
        if (m_screen != nullptr && m_screen->captured() == this)
            m_screen->release_capture();
    }

} // namespace volt::ui
