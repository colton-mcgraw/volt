#include <ui/widgets/progress_bar.hpp>

#include <ui/painter.hpp>
#include <ui/theme.hpp>

#include <algorithm>
#include <cmath>

namespace volt::ui
{

    void progress_bar::set_value(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        if (value == m_value)
            return;
        m_value = value;
        invalidate();
    }

    void progress_bar::set_indeterminate(bool indeterminate)
    {
        if (indeterminate == m_indeterminate)
            return;
        m_indeterminate = indeterminate;
        if (indeterminate)
            animate();
        invalidate();
    }

    void progress_bar::set_text(std::string text)
    {
        if (text == m_text)
            return;
        m_text = std::move(text);
        invalidate();
    }

    void progress_bar::on_attached()
    {
        if (m_indeterminate)
            animate();
    }

    bool progress_bar::on_animate(float seconds)
    {
        if (!m_indeterminate || !visible())
            return false;
        m_phase = std::fmod(m_phase + seconds * 0.8f, 1.0f);
        invalidate();
        return true;
    }

    math::vec2f progress_bar::on_measure(const constraints &)
    {
        return {120.0f, m_text.empty() ? 6.0f : theme().control_height - 6.0f};
    }

    void progress_bar::on_paint(painter &p)
    {
        const ui::theme &t = theme();
        const rect &b = bounds();
        const float radius = std::min(t.radius, b.height * 0.5f);

        p.fill(b, t.track, radius);
        {
            clip_scope clip(p, b);
            if (m_indeterminate)
            {
                const float width = b.width * 0.3f;
                const float x = b.x - width + (b.width + width) * m_phase;
                p.fill({x, b.y, width, b.height}, t.accent, radius);
            }
            else if (m_value > 0.0f)
            {
                p.fill({b.x, b.y, b.width * m_value, b.height}, t.accent, radius);
            }
        }
        if (!m_text.empty())
            p.text(b, m_text, t.small_style(), t.text, align::center);
    }

} // namespace volt::ui
