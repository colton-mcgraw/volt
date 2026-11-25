#include <ui/widgets/label.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <cmath>

namespace volt::ui
{

    label::label(std::string text, text_role role) : m_text(std::move(text)), m_role(role)
    {
        set_hit_testable(false);
    }

    void label::set_text(std::string text)
    {
        if (text == m_text)
            return;
        m_text = std::move(text);
        invalidate_layout();
    }

    void label::set_role(text_role role)
    {
        m_role = role;
        invalidate();
    }

    void label::set_size(text_size size)
    {
        m_size = size;
        invalidate_layout();
    }

    void label::set_color(std::optional<math::vec4f> color)
    {
        m_color = color;
        invalidate();
    }

    void label::set_align(align horizontal)
    {
        m_align = horizontal;
        invalidate();
    }

    void label::set_wrap(bool wrap, std::size_t max_lines)
    {
        m_wrap = wrap;
        m_max_lines = max_lines;
        invalidate_layout();
    }

    text_style label::style() const
    {
        const ui::theme &t = theme();
        switch (m_size)
        {
        case text_size::small:
            return t.small_style();
        case text_size::title:
            return t.title_style();
        case text_size::body:
            break;
        }
        return t.body_style();
    }

    math::vec4f label::color() const
    {
        const ui::theme &t = theme();
        if (!is_enabled())
            return t.text_disabled;
        if (m_color)
            return *m_color;
        switch (m_role)
        {
        case text_role::dim:
            return t.text_dim;
        case text_role::accent:
            return t.accent;
        case text_role::on_accent:
            return t.text_on_accent;
        case text_role::normal:
            break;
        }
        return t.text;
    }

    math::vec2f label::on_measure(const constraints &limits)
    {
        const text_style s = style();
        const text_metrics metrics = fonts().metrics(s);
        const float line = std::ceil(metrics.line_height);

        if (m_wrap && std::isfinite(limits.max.x()))
        {
            font::layout_options options;
            options.max_width = limits.max.x();
            options.max_lines = m_max_lines;
            const font::text_layout laid = fonts().layout(m_text, s, options);
            return {std::ceil(laid.extent.x()), std::max(line, std::ceil(laid.extent.y()))};
        }
        return {std::ceil(fonts().measure(m_text, s)), line};
    }

    void label::on_paint(painter &p)
    {
        const text_style s = style();
        if (!m_wrap)
        {
            p.text(bounds(), m_text, s, color(), m_align);
            return;
        }

        font::layout_options options;
        options.max_width = bounds().width;
        options.max_lines = m_max_lines;
        options.align = m_align == align::center ? font::text_align::center
                        : m_align == align::end  ? font::text_align::right
                                                 : font::text_align::left;
        const font::text_layout laid = fonts().layout(m_text, s, options);
        p.layout(laid, s, bounds().position(), color());
    }

} // namespace volt::ui
