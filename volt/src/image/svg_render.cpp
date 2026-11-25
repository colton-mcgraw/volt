#include <image/svg.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace volt::image::svg
{

    void draw(const document &source, canvas &target, const math::mat3f &transform, float tolerance)
    {
        tolerance = std::max(tolerance, 0.001f);
        std::vector<float> opacities; // of the open groups
        for (const item &entry : source.items)
        {
            switch (entry.type)
            {
            case item::kind::begin_group:
                target.push_layer();
                opacities.push_back(entry.opacity);
                break;
            case item::kind::end_group:
                if (!opacities.empty())
                {
                    target.pop_layer(opacities.back());
                    opacities.pop_back();
                }
                break;
            case item::kind::shape:
            {
                const shape &s = entry.shape;
                const math::mat3f device = transform * s.transform;
                if (s.fill.type != paint::kind::none)
                {
                    const std::vector<contour> polygons = flatten(s.path, device, tolerance);
                    target.fill(polygons, s.fill_rule, s.fill, device, s.fill_opacity);
                }
                if (s.stroke.type != paint::kind::none && s.stroke_style.width > 0.0f)
                {
                    // Stroked in user space, so that a non-uniform transform
                    // shapes the pen too; then onto the device.
                    const float scale = max_scale(device);
                    if (!(scale > 0.0f) || !std::isfinite(scale))
                        break;
                    std::vector<contour> outline = stroke(s.path, s.stroke_style, tolerance / scale);
                    for (contour &c : outline)
                        for (math::vec2f &p : c.points)
                            p = apply(device, p);
                    target.fill(outline, fill_rule::nonzero, s.stroke, device, s.stroke_opacity);
                }
                break;
            }
            }
        }
        while (!opacities.empty())
        {
            target.pop_layer(opacities.back());
            opacities.pop_back();
        }
    }

    std::expected<image, std::string> rasterize(const document &source, const rasterize_options &options)
    {
        const float w = source.width;
        const float h = source.height;
        if (!(w > 0.0f) || !(h > 0.0f))
            return std::unexpected(std::format("the SVG has no size ({} x {})", w, h));

        float scale = options.scale;
        float width = 0.0f;
        float height = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        if (options.width != 0 && options.height != 0)
        {
            width = static_cast<float>(options.width);
            height = static_cast<float>(options.height);
            scale = std::min(width / w, height / h);
            x = (width - w * scale) / 2.0f;
            y = (height - h * scale) / 2.0f;
        }
        else if (options.width != 0)
        {
            width = static_cast<float>(options.width);
            scale = width / w;
            height = std::max(1.0f, std::round(h * scale));
        }
        else if (options.height != 0)
        {
            height = static_cast<float>(options.height);
            scale = height / h;
            width = std::max(1.0f, std::round(w * scale));
        }
        else
        {
            if (!(scale > 0.0f) || !std::isfinite(scale))
                return std::unexpected(std::format("the SVG cannot be drawn at a scale of {}", scale));
            const float longest = std::max(w, h) * scale;
            if (longest > rasterize_options::max_intrinsic)
                scale *= rasterize_options::max_intrinsic / longest;
            width = std::max(1.0f, std::round(w * scale));
            height = std::max(1.0f, std::round(h * scale));
        }
        if (!(width < 1e6f) || !(height < 1e6f))
            return std::unexpected(std::format("the SVG would be {} x {} pixels", width, height));

        auto target = canvas::create(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                     options.linear_light);
        if (!target)
            return std::unexpected(std::move(target.error()));
        const math::mat3f placement = math::mat3f::from_rows({{scale, 0, x}, {0, scale, y}, {0, 0, 1}});
        draw(source, *target, placement, options.tolerance);
        return target->to_image();
    }

} // namespace volt::image::svg
