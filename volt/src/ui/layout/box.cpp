#include <ui/layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace volt::ui
{
    namespace
    {
        // Picks the component along (0) or across (1) the axis.
        struct axes
        {
            std::size_t main;
            std::size_t cross;
        };

        axes axes_of(orientation o) noexcept
        {
            return o == orientation::horizontal ? axes{0, 1} : axes{1, 0};
        }

        math::vec2f compose(axes a, float main, float cross) noexcept
        {
            math::vec2f v{};
            v[a.main] = main;
            v[a.cross] = cross;
            return v;
        }
    } // namespace

    void box::set_spacing(float spacing)
    {
        m_spacing = spacing;
        invalidate_layout();
    }

    void box::set_padding(const insets &padding)
    {
        m_padding = padding;
        invalidate_layout();
    }

    void box::set_main_align(align a)
    {
        m_main = a;
        invalidate_layout();
    }

    void box::set_cross_align(align a)
    {
        m_cross = a;
        invalidate_layout();
    }

    math::vec2f box::on_measure(const constraints &limits)
    {
        const axes a = axes_of(m_orientation);
        const constraints inner = limits.deflate(m_padding);
        const float max_main = inner.max[a.main];
        const float max_cross = inner.max[a.cross];

        float main = 0.0f;
        float cross = 0.0f;
        int shown = 0;
        for (const auto &child : children())
        {
            if (!child->visible())
                continue;
            ++shown;

            // A stretched child is offered the full depth as its minimum, so
            // that something which fills (a scroll area) knows it may.
            const align across = child->layout().cross.value_or(m_cross);
            constraints offered;
            offered.max = compose(a, max_main, max_cross);
            if (across == align::stretch && std::isfinite(inner.min[a.cross]))
                offered.min[a.cross] = inner.min[a.cross];

            const math::vec2f size = child->measure(offered);
            main += size[a.main];
            cross = std::max(cross, size[a.cross]);
        }
        if (shown > 1)
            main += m_spacing * static_cast<float>(shown - 1);

        return compose(a, main, cross) + m_padding.total();
    }

    void box::on_arrange(const rect &bounds)
    {
        const axes a = axes_of(m_orientation);
        const rect content = bounds.inset(m_padding);
        const math::vec2f origin = content.position();
        const math::vec2f extent = content.size();

        std::vector<widget *> shown;
        float fixed = 0.0f;
        float weights = 0.0f;
        for (const auto &child : children())
        {
            if (!child->visible())
            {
                child->arrange({origin.x(), origin.y(), 0.0f, 0.0f});
                continue;
            }
            shown.push_back(child.get());
            if (child->layout().flex > 0.0f)
                weights += child->layout().flex;
            else
                fixed += child->measured()[a.main];
        }
        if (shown.empty())
            return;

        const float gaps = m_spacing * static_cast<float>(shown.size() - 1);
        const float free = std::max(0.0f, extent[a.main] - fixed - gaps);

        float position = origin[a.main];
        if (weights <= 0.0f && free > 0.0f)
        {
            if (m_main == align::center)
                position += std::floor(free * 0.5f);
            else if (m_main == align::end)
                position += free;
        }

        for (widget *child : shown)
        {
            const layout_params &params = child->layout();
            const math::vec2f measured = child->measured();

            float length = measured[a.main];
            if (params.flex > 0.0f && weights > 0.0f)
                length = std::max(free * params.flex / weights, params.min_size[a.main]);

            const align across = params.cross.value_or(m_cross);
            float depth = across == align::stretch ? extent[a.cross] : std::min(measured[a.cross], extent[a.cross]);
            depth = std::min(depth, params.max_size[a.cross]);
            float offset = 0.0f;
            if (across == align::center)
                offset = std::floor((extent[a.cross] - depth) * 0.5f);
            else if (across == align::end)
                offset = extent[a.cross] - depth;

            const math::vec2f at = compose(a, position, origin[a.cross] + offset);
            const math::vec2f size = compose(a, length, depth);
            child->arrange(rect::from(at, size));
            position += length + m_spacing;
        }
    }

} // namespace volt::ui
