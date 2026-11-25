#include <ui/draw_list.hpp>

#include <cassert>

namespace volt::ui
{

    void draw_list::clear()
    {
        m_items.clear();
        m_clips.clear();
        m_stack.clear();
        m_clips.push_back(no_clip);
        m_stack.push_back(0);
    }

    void draw_list::add(draw_item item, const rect &extent)
    {
        if (!extent.intersects(current_clip()))
            return;
        item.clip = m_stack.back();
        m_items.push_back(item);
    }

    void draw_list::rectangle(const rect &area, const math::vec4f &color, float radius)
    {
        if (area.empty() || color[3] <= 0.0f)
            return;

        draw_item item;
        item.kind = shape_kind::rectangle;
        item.bounds = area;
        item.color = color;
        item.radius = radius;
        add(item, area);
    }

    void draw_list::rectangle(const rect &area, const math::vec4f &fill, float radius, float border,
                              const math::vec4f &border_color)
    {
        if (area.empty() || (fill[3] <= 0.0f && (border <= 0.0f || border_color[3] <= 0.0f)))
            return;

        draw_item item;
        item.kind = shape_kind::rectangle;
        item.bounds = area;
        item.color = fill;
        item.radius = radius;
        item.border = border;
        item.border_color = border_color;
        add(item, area);
    }

    void draw_list::shadow(const rect &area, float radius, float blur, const math::vec4f &color)
    {
        if (area.empty() || color[3] <= 0.0f)
            return;

        draw_item item;
        item.kind = shape_kind::shadow;
        item.bounds = area;
        item.color = color;
        item.radius = radius;
        item.extra = blur;
        add(item, area.outset(blur));
    }

    void draw_list::line(const math::vec2f &from, const math::vec2f &to, float width, const math::vec4f &color)
    {
        if (width <= 0.0f || color[3] <= 0.0f)
            return;

        draw_item item;
        item.kind = shape_kind::line;
        item.from = from;
        item.to = to;
        item.bounds = rect::between(from, to);
        item.color = color;
        item.extra = width;
        add(item, item.bounds.outset(width * 0.5f + 1.0f));
    }

    void draw_list::polyline(std::span<const math::vec2f> points, float width, const math::vec4f &color)
    {
        for (std::size_t i = 1; i < points.size(); ++i)
            line(points[i - 1], points[i], width, color);
    }

    void draw_list::image(const rect &area, texture_id texture, const math::vec4f &tint, float radius,
                          const math::vec2f &uv_min, const math::vec2f &uv_max)
    {
        if (area.empty() || !texture.valid() || tint[3] <= 0.0f)
            return;

        draw_item item;
        item.kind = shape_kind::image;
        item.bounds = area;
        item.color = tint;
        item.radius = radius;
        item.texture = texture;
        item.uv_min = uv_min;
        item.uv_max = uv_max;
        add(item, area);
    }

    void draw_list::glyph(const rect &area, texture_id page, const math::vec2f &uv_min, const math::vec2f &uv_max,
                          const math::vec4f &color, float contrast)
    {
        if (area.empty() || color[3] <= 0.0f)
            return;

        draw_item item;
        item.kind = shape_kind::glyph;
        item.bounds = area;
        item.color = color;
        item.texture = page;
        item.uv_min = uv_min;
        item.uv_max = uv_max;
        item.extra = contrast;
        add(item, area);
    }

    void draw_list::push_clip(const rect &area)
    {
        m_clips.push_back(current_clip().intersect(area));
        m_stack.push_back(static_cast<std::uint32_t>(m_clips.size() - 1));
    }

    void draw_list::pop_clip()
    {
        assert(m_stack.size() > 1 && "pop_clip without push_clip");
        if (m_stack.size() > 1)
            m_stack.pop_back();
    }

} // namespace volt::ui
